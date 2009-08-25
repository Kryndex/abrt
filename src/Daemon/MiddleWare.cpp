/*
    MiddleWare.cpp

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    */

#include "abrtlib.h"
#include "Settings.h"
#include "DebugDump.h"
#include "ABRTException.h"
#include "CommLayerInner.h"
#include "MiddleWare.h"


/**
 * An instance of CPluginManager. When MiddleWare wants to do something
 * with plugins, it calls the plugin manager.
 * @see PluginManager.h
 */
CPluginManager* g_pPluginManager;


/**
 * An instance of CRPM used for package checking.
 * @see RPM.h
 */
static CRPM s_RPM;
/**
 * A set of blacklisted packages.
 */
static set_strings_t s_setBlackList;
/**
 * A map, which associates particular analyzer to one or more
 * action or reporter plugins. These are activated when a crash, which
 * is maintained by particular analyzer, occurs.
 */
static map_analyzer_actions_and_reporters_t s_mapAnalyzerActionsAndReporters;
/**
 * A vector of one or more action or reporter plugins. These are
 * activated when any crash occurs.
 */
static vector_pair_string_string_t s_vectorActionsAndReporters;


static void RunAnalyzerActions(const std::string& pAnalyzer, const std::string& pDebugDumpDir);


/**
 * Transforms a debugdump direcortry to inner crash
 * report form. This form is used for later reporting.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @param pCrashReport A created crash report.
 */
static void DebugDumpToCrashReport(const std::string& pDebugDumpDir, map_crash_report_t& pCrashReport)
{
    std::string fileName;
    std::string content;
    bool isTextFile;
    CDebugDump dd;
    dd.Open(pDebugDumpDir);

    if (!dd.Exist(FILENAME_ARCHITECTURE) ||
        !dd.Exist(FILENAME_KERNEL) ||
        !dd.Exist(FILENAME_PACKAGE) ||
        !dd.Exist(FILENAME_COMPONENT) ||
        !dd.Exist(FILENAME_RELEASE) ||
        !dd.Exist(FILENAME_EXECUTABLE))
    {
        dd.Close();
        throw CABRTException(EXCEP_ERROR, "DebugDumpToCrashReport(): One or more of important file(s)'re missing.");
    }
    pCrashReport.clear();
    dd.InitGetNextFile();
    while (dd.GetNextFile(fileName, content, isTextFile))
    {
        if (!isTextFile)
        {
            add_crash_data_to_crash_report(pCrashReport,
                                           fileName,
                                           CD_BIN,
                                           CD_ISNOTEDITABLE,
                                           pDebugDumpDir + "/" + fileName);
        }
        else
        {
            if (fileName == FILENAME_ARCHITECTURE ||
                fileName == FILENAME_KERNEL ||
                fileName == FILENAME_PACKAGE ||
                fileName == FILENAME_COMPONENT ||
                fileName == FILENAME_RELEASE ||
                fileName == FILENAME_EXECUTABLE)
            {
                add_crash_data_to_crash_report(pCrashReport, fileName, CD_TXT, CD_ISNOTEDITABLE, content);
            }
            else if (fileName != FILENAME_UID &&
                     fileName != FILENAME_ANALYZER &&
                     fileName != FILENAME_TIME &&
                     fileName != FILENAME_DESCRIPTION )
            {
                if (content.length() < CD_ATT_SIZE)
                {
                    add_crash_data_to_crash_report(pCrashReport, fileName, CD_TXT, CD_ISEDITABLE, content);
                }
                else
                {
                    add_crash_data_to_crash_report(pCrashReport, fileName, CD_ATT, CD_ISEDITABLE, content);
                }
            }
        }
    }
    dd.Close();
}

/**
 * Get a local UUID from particular analyzer plugin.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return A local UUID.
 */
static std::string GetLocalUUID(const std::string& pAnalyzer,
                                      const std::string& pDebugDumpDir)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(pAnalyzer);
    return analyzer->GetLocalUUID(pDebugDumpDir);
}

/**
 * Get a global UUID from particular analyzer plugin.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return A global UUID.
 */
static std::string GetGlobalUUID(const std::string& pAnalyzer,
                                       const std::string& pDebugDumpDir)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(pAnalyzer);
    return analyzer->GetGlobalUUID(pDebugDumpDir);
}

/**
 * Take care of getting all additional data needed
 * for computing UUIDs and creating a report for particular analyzer
 * plugin. This report could be send somewhere afterwards.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpPath A debugdump dir containing all necessary data.
 */
static void CreateReport(const std::string& pAnalyzer,
                               const std::string& pDebugDumpDir)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(pAnalyzer);
    analyzer->CreateReport(pDebugDumpDir);
}

mw_result_t CreateCrashReport(const std::string& pUUID,
                                                        const std::string& pUID,
                                                        map_crash_report_t& pCrashReport)
{
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase);
    database_row_t row;
    database->Connect();
    row = database->GetUUIDData(pUUID, pUID);
    database->DisConnect();
    CDebugDump dd;

    if (pUUID == "" || row.m_sUUID != pUUID)
    {
        comm_layer_inner_warning("CreateCrashReport(): UUID '"+pUUID+"' is not in database.");
        return MW_IN_DB_ERROR;
    }

    try
    {
        std::string analyzer;
        std::string gUUID;

        dd.Open(row.m_sDebugDumpDir);
        dd.LoadText(FILENAME_ANALYZER, analyzer);
        dd.Close();

        CreateReport(analyzer, row.m_sDebugDumpDir);

        gUUID = GetGlobalUUID(analyzer, row.m_sDebugDumpDir);

        RunAnalyzerActions(analyzer, row.m_sDebugDumpDir);
        DebugDumpToCrashReport(row.m_sDebugDumpDir, pCrashReport);

        add_crash_data_to_crash_report(pCrashReport, CD_UUID, CD_TXT, CD_ISNOTEDITABLE, gUUID);
        add_crash_data_to_crash_report(pCrashReport, CD_MWANALYZER, CD_SYS, CD_ISNOTEDITABLE, analyzer);
        add_crash_data_to_crash_report(pCrashReport, CD_MWUID, CD_SYS, CD_ISNOTEDITABLE, pUID);
        add_crash_data_to_crash_report(pCrashReport, CD_MWUUID, CD_SYS, CD_ISNOTEDITABLE, pUUID);
        add_crash_data_to_crash_report(pCrashReport, CD_COMMENT, CD_TXT, CD_ISEDITABLE, "");
        add_crash_data_to_crash_report(pCrashReport, CD_REPRODUCE, CD_TXT, CD_ISEDITABLE, "1.\n2.\n3.\n");
    }
    catch (CABRTException& e)
    {
        comm_layer_inner_warning("CreateCrashReport(): " + e.what());
        if (e.type() == EXCEP_DD_OPEN)
        {
            return MW_ERROR;
        }
        else if (e.type() == EXCEP_DD_LOAD)
        {
            return MW_FILE_ERROR;
        }
        else if (e.type() == EXCEP_PLUGIN)
        {
            return MW_PLUGIN_ERROR;
        }
        return MW_CORRUPTED;
    }

    return MW_OK;
}

void RunAction(const std::string& pActionDir,
                            const std::string& pPluginName,
                            const std::string& pPluginArgs)
{
    try
    {
        CAction* action = g_pPluginManager->GetAction(pPluginName);

        action->Run(pActionDir, pPluginArgs);
    }
    catch (CABRTException& e)
    {
        comm_layer_inner_warning("RunAction(): " + e.what());
        comm_layer_inner_status("Execution of '"+pPluginName+"' was not successful: " + e.what());
    }

}

void RunActionsAndReporters(const std::string& pDebugDumpDir)
{
    vector_pair_string_string_t::iterator it_ar;
    for (it_ar = s_vectorActionsAndReporters.begin(); it_ar != s_vectorActionsAndReporters.end(); it_ar++)
    {
        try
        {
            if (g_pPluginManager->GetPluginType((*it_ar).first) == REPORTER)
            {
                CReporter* reporter = g_pPluginManager->GetReporter((*it_ar).first);

                map_crash_report_t crashReport;
                DebugDumpToCrashReport(pDebugDumpDir, crashReport);
                reporter->Report(crashReport, (*it_ar).second);
            }
            else if (g_pPluginManager->GetPluginType((*it_ar).first) == ACTION)
            {
                CAction* action = g_pPluginManager->GetAction((*it_ar).first);
                action->Run(pDebugDumpDir, (*it_ar).second);
            }
        }
        catch (CABRTException& e)
        {
            comm_layer_inner_warning("RunActionsAndReporters(): " + e.what());
            comm_layer_inner_status("Activation of plugin '"+(*it_ar).first+"' was not successful: " + e.what());
        }
    }
}

report_status_t Report(const map_crash_report_t& pCrashReport,
                                    const std::string& pUID)
{
    report_status_t ret;
    std::string ret_key;
    std::string message;
    if (pCrashReport.find(CD_MWANALYZER) == pCrashReport.end() ||
        pCrashReport.find(CD_MWUID) == pCrashReport.end() ||
        pCrashReport.find(CD_MWUUID) == pCrashReport.end())
    {
        throw CABRTException(EXCEP_ERROR, "Report(): System data are missing in crash report.");
    }

    std::string analyzer = pCrashReport.find(CD_MWANALYZER)->second[CD_CONTENT];
    std::string UID = pCrashReport.find(CD_MWUID)->second[CD_CONTENT];
    std::string UUID = pCrashReport.find(CD_MWUUID)->second[CD_CONTENT];
    std::string packageNVR = pCrashReport.find(FILENAME_PACKAGE)->second[CD_CONTENT];
    std::string packageName = packageNVR.substr(0, packageNVR.rfind("-", packageNVR.rfind("-") - 1 ));

    // ii = 0 -> analyzer is without package name (default)
    // ii = 1 -> analyzer is with package name (CCpp:xrog-x11-app) (additional reporters to package)
    int ii;
    for (ii = 0; ii < 2; ii++)
    {
        if (ii == 1)
        {
            analyzer += ":" + packageName;
        }

        if (s_mapAnalyzerActionsAndReporters.find(analyzer) != s_mapAnalyzerActionsAndReporters.end())
        {
            vector_pair_string_string_t::iterator it_r = s_mapAnalyzerActionsAndReporters[analyzer].begin();
            for (; it_r != s_mapAnalyzerActionsAndReporters[analyzer].end(); it_r++)
            {
                try
                {
                    std::string res;

                    ret_key = (*it_r).first;
                    if (ii == 1)
                    {
                        ret_key += " (" + packageName + ")";
                    }
                    if (g_pPluginManager->GetPluginType((*it_r).first) == REPORTER)
                    {
                        CReporter* reporter = g_pPluginManager->GetReporter((*it_r).first);
                        std::string home = "";
                        map_plugin_settings_t oldSettings;
                        map_plugin_settings_t newSettings;

                        if (pUID != "")
                        {
                            home = get_home_dir(atoi(pUID.c_str()));
                            if (home != "")
                            {
                                oldSettings = reporter->GetSettings();

                                if (LoadPluginSettings(home + "/.abrt/" + (*it_r).first + "."PLUGINS_CONF_EXTENSION, newSettings))
                                {
                                    reporter->SetSettings(newSettings);
                                }
                            }
                        }

                        res = reporter->Report(pCrashReport, (*it_r).second);

                        if (home != "")
                        {
                            reporter->SetSettings(oldSettings);
                        }
                    }
                    ret[ret_key].push_back("1");
                    ret[ret_key].push_back(res);
                    message += res + "\n";
                }
                catch (CABRTException& e)
                {
                    ret[ret_key].push_back("0");
                    ret[ret_key].push_back(e.what());
                    comm_layer_inner_warning("Report(): " + e.what());
                    comm_layer_inner_status("Reporting via '"+(*it_r).first+"' was not successful: " + e.what());
                }
            }
        }
    }

    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase);
    database->Connect();
    database->SetReported(UUID, UID, message);
    database->DisConnect();

    return ret;
}

void DeleteDebugDumpDir(const std::string& pDebugDumpDir)
{
    CDebugDump dd;
    dd.Open(pDebugDumpDir);
    dd.Delete();
    dd.Close();
}

std::string DeleteCrashInfo(const std::string& pUUID,
                                         const std::string& pUID)
{
    database_row_t row;
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase);
    database->Connect();
    row = database->GetUUIDData(pUUID, pUID);
    database->Delete(pUUID, pUID);
    database->DisConnect();

    return row.m_sDebugDumpDir;
}

/**
 * Check whether particular debugdump directory is saved
 * in database. This check is done together with an UID of an user.
 * @param pUID an UID of an user.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return It returns true if debugdump dir is already saved, otherwise
 * it returns false.
 */
static bool IsDebugDumpSaved(const std::string& pUID,
                                   const std::string& pDebugDumpDir)
{
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase);
    vector_database_rows_t rows;
    database->Connect();
    rows = database->GetUIDData(pUID);
    database->DisConnect();

    int ii;
    bool found = false;
    for (ii = 0; ii < rows.size(); ii++)
    {
        if (rows[ii].m_sDebugDumpDir == pDebugDumpDir)
        {
            found = true;
            break;
        }
    }

    return found;
}

/**
 * Get a package name from executable name and save
 * package description to particular debugdump directory of a crash.
 * @param pExecutable A name of crashed application.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return It return results of operation. See mw_result_t.
 */
static mw_result_t SavePackageDescriptionToDebugDump(const std::string& pExecutable,
                                                                        const std::string& pDebugDumpDir)
{
    std::string package;
    std::string packageName;

    if (pExecutable == "kernel")
    {
        packageName = package = "kernel";
    }
    else
    {
        package = s_RPM.GetPackage(pExecutable);
        packageName = package.substr(0, package.rfind("-", package.rfind("-") - 1));
        if (packageName == "" ||
            (s_setBlackList.find(packageName) != s_setBlackList.end()))
        {
            if (packageName == "")
            {
                error_msg("Executable doesn't belong to any package");
                return MW_PACKAGE_ERROR;
            }
            log("Blacklisted package");
            return MW_BLACKLISTED;
        }
        if (g_settings_bOpenGPGCheck)
        {
            if (!s_RPM.CheckFingerprint(packageName))
            {
                error_msg("package isn't signed with proper key");
                return MW_GPG_ERROR;
            }
            if (!s_RPM.CheckHash(packageName, pExecutable))
            {
                error_msg("executable has bad hash");
                return MW_GPG_ERROR;
            }
        }
    }

    std::string description = s_RPM.GetDescription(packageName);
    std::string component = s_RPM.GetComponent(pExecutable);

    CDebugDump dd;
    try
    {
        dd.Open(pDebugDumpDir);
        dd.SaveText(FILENAME_PACKAGE, package);
        dd.SaveText(FILENAME_DESCRIPTION, description);
        dd.SaveText(FILENAME_COMPONENT, component);
        dd.Close();
    }
    catch (CABRTException& e)
    {
        comm_layer_inner_warning("SavePackageDescriptionToDebugDump(): " + e.what());
        if (e.type() == EXCEP_DD_SAVE)
        {
            dd.Close();
            return MW_FILE_ERROR;
        }
        return MW_ERROR;
    }

    return MW_OK;
}

/**
 * Execute all action plugins, which are associated to
 * particular analyzer plugin.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpPath A debugdump dir containing all necessary data.
 */
static void RunAnalyzerActions(const std::string& pAnalyzer, const std::string& pDebugDumpDir)
{
    if (s_mapAnalyzerActionsAndReporters.find(pAnalyzer) != s_mapAnalyzerActionsAndReporters.end())
    {
        vector_pair_string_string_t::iterator it_a;
        for (it_a = s_mapAnalyzerActionsAndReporters[pAnalyzer].begin();
             it_a != s_mapAnalyzerActionsAndReporters[pAnalyzer].end();
             it_a++)
        {
            try
            {
                if (g_pPluginManager->GetPluginType((*it_a).first) == ACTION)
                {
                    CAction* action = g_pPluginManager->GetAction((*it_a).first);

                    action->Run(pDebugDumpDir, (*it_a).second);
                }
            }
            catch (CABRTException& e)
            {
                comm_layer_inner_warning("RunAnalyzerActions(): " + e.what());
                comm_layer_inner_status("Action performed by '"+(*it_a).first+"' was not successful: " + e.what());
            }
        }
    }
}

/**
 * Save a debugdump into database. If saving is
 * successful, then crash info is filled. Otherwise the crash info is
 * not changed.
 * @param pUUID A local UUID of a crash.
 * @param pUID An UID of an user.
 * @param pTime Time when a crash occurs.
 * @param pDebugDumpPath A debugdump path.
 * @param pCrashInfo A filled crash info.
 * @return It return results of operation. See mw_result_t.
 */
static mw_result_t SaveDebugDumpToDatabase(const std::string& pUUID,
                                                              const std::string& pUID,
                                                              const std::string& pTime,
                                                              const std::string& pDebugDumpDir,
                                                              map_crash_info_t& pCrashInfo)
{
    mw_result_t res;
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase);
    database_row_t row;
    database->Connect();
    database->Insert(pUUID, pUID, pDebugDumpDir, pTime);
    row = database->GetUUIDData(pUUID, pUID);
    database->DisConnect();
    res = GetCrashInfo(pUUID, pUID, pCrashInfo);
    if (row.m_sReported == "1")
    {
        log("Crash is already reported");
        return MW_REPORTED;
    }
    if (row.m_sCount != "1")
    {
        log("Crash is in database already");
        return MW_OCCURED;
    }
    return res;
}

mw_result_t SaveDebugDump(const std::string& pDebugDumpDir)
{
    map_crash_info_t info;
    return SaveDebugDump(pDebugDumpDir, info);
}

mw_result_t SaveDebugDump(const std::string& pDebugDumpDir,
                                                    map_crash_info_t& pCrashInfo)
{
    std::string lUUID;
    std::string UID;
    std::string time;
    std::string analyzer;
    std::string executable;
    CDebugDump dd;
    mw_result_t res;

    try
    {
        dd.Open(pDebugDumpDir);
        dd.LoadText(FILENAME_TIME, time);
        dd.LoadText(FILENAME_UID, UID);
        dd.LoadText(FILENAME_ANALYZER, analyzer);
        dd.LoadText(FILENAME_EXECUTABLE, executable);
        dd.Close();
    }
    catch (CABRTException& e)
    {
        comm_layer_inner_warning("SaveDebugDump(): " + e.what());
        if (e.type() == EXCEP_DD_SAVE)
        {
            dd.Close();
            return MW_FILE_ERROR;
        }
        return MW_ERROR;
    }

    if (IsDebugDumpSaved(UID, pDebugDumpDir))
    {
        return MW_IN_DB;
    }
    if ((res = SavePackageDescriptionToDebugDump(executable, pDebugDumpDir)) != MW_OK)
    {
        return res;
    }

    lUUID = GetLocalUUID(analyzer, pDebugDumpDir);

    return SaveDebugDumpToDatabase(lUUID, UID, time, pDebugDumpDir, pCrashInfo);
}

mw_result_t GetCrashInfo(const std::string& pUUID,
                                                   const std::string& pUID,
                                                   map_crash_info_t& pCrashInfo)
{
    pCrashInfo.clear();
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase);
    database_row_t row;
    database->Connect();
    row = database->GetUUIDData(pUUID, pUID);
    database->DisConnect();

    CDebugDump dd;
    std::string package;
    std::string executable;
    std::string description;

    try
    {
        dd.Open(row.m_sDebugDumpDir);
        dd.LoadText(FILENAME_EXECUTABLE, executable);
        dd.LoadText(FILENAME_PACKAGE, package);
        dd.LoadText(FILENAME_DESCRIPTION, description);
        dd.Close();
    }
    catch (CABRTException& e)
    {
        comm_layer_inner_warning("GetCrashInfo(): " + e.what());
        if (e.type() == EXCEP_DD_LOAD)
        {
            dd.Close();
            return MW_FILE_ERROR;
        }
        return MW_ERROR;
    }
    add_crash_data_to_crash_info(pCrashInfo, CD_EXECUTABLE, executable);
    add_crash_data_to_crash_info(pCrashInfo, CD_PACKAGE, package);
    add_crash_data_to_crash_info(pCrashInfo, CD_DESCRIPTION, description);
    add_crash_data_to_crash_info(pCrashInfo, CD_UUID, row.m_sUUID);
    add_crash_data_to_crash_info(pCrashInfo, CD_UID, row.m_sUID);
    add_crash_data_to_crash_info(pCrashInfo, CD_COUNT, row.m_sCount);
    add_crash_data_to_crash_info(pCrashInfo, CD_TIME, row.m_sTime);
    add_crash_data_to_crash_info(pCrashInfo, CD_REPORTED, row.m_sReported);
    add_crash_data_to_crash_info(pCrashInfo, CD_MESSAGE, row.m_sMessage);
    add_crash_data_to_crash_info(pCrashInfo, CD_MWDDD, row.m_sDebugDumpDir);

    return MW_OK;
}

vector_pair_string_string_t GetUUIDsOfCrash(const std::string& pUID)
{
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase);
    vector_database_rows_t rows;
    database->Connect();
    rows = database->GetUIDData(pUID);
    database->DisConnect();

    vector_pair_string_string_t UUIDsUIDs;
    int ii;
    for (ii = 0; ii < rows.size(); ii++)
    {
        UUIDsUIDs.push_back(make_pair(rows[ii].m_sUUID, rows[ii].m_sUID));
    }

    return UUIDsUIDs;
}

void AddOpenGPGPublicKey(const std::string& pKey)
{
    s_RPM.LoadOpenGPGPublicKey(pKey);
}

void AddBlackListedPackage(const std::string& pPackage)
{
    s_setBlackList.insert(pPackage);
}

void AddAnalyzerActionOrReporter(const std::string& pAnalyzer,
                                              const std::string& pAnalyzerOrReporter,
                                              const std::string& pArgs)
{
    s_mapAnalyzerActionsAndReporters[pAnalyzer].push_back(make_pair(pAnalyzerOrReporter, pArgs));
}

void AddActionOrReporter(const std::string& pActionOrReporter,
                                      const std::string& pArgs)
{
    s_vectorActionsAndReporters.push_back(make_pair(pActionOrReporter, pArgs));
}
