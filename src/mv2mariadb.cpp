/*
	mv2mariadb - convert MediathekView db to mysql
	Copyright (C) 2015-2017, M. Liebmann 'micha-bbg'

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public
	License along with this program; if not, write to the
	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
	Boston, MA  02110-1301, USA.
*/

#define PROGVERSION "0.3.8"
#define DBVERSION "3.0"
#define PROGNAME "mv2mariadb"
#define DEFAULTXZ "mv-movielist.xz"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <errno.h>
#include <semaphore.h>

#include <iostream>
#include <fstream>
#include <climits>

#include <json/json.h>

#include "sql.h"
#include "mv2mariadb.h"
#include "common/helpers.h"
#include "common/filehelpers.h"
#include "lzma_dec.h"
#include "curl.h"
#include "serverlist.h"

#define DB_1 "-1.json"
#define DB_2 "-2.json"

CMV2Mysql*		g_mainInstance;
GSettings		g_settings;
bool			g_debugPrint;
const char*		g_progName;
const char*		g_progCopyright;
const char*		g_progVersion;
const char*		g_dbVersion;
string			g_mvVersion;
time_t			g_mvDate;
string			g_passwordFile;

void myExit(int val);

CMV2Mysql::CMV2Mysql()
: configFile('\t')
{
	Init();
}

void CMV2Mysql::Init()
{
	g_progName		= PROGNAME;
	g_progCopyright		= "Copyright (C) 2015-2017, M. Liebmann 'micha-bbg'";
	g_progVersion		= "v" PROGVERSION;
	g_dbVersion		= DBVERSION;
	defaultXZ		= (string)DEFAULTXZ;

	epoch			= 0; /* all data */
	cronMode		= 0;
	cronModeEcho		= false;
	g_debugPrint		= false;
	multiQuery		= true;
	downloadOnly		= false;
	createIndexes		= true;
	loadServerlist		= false;
	g_mvDate		= time(0);
	csql			= NULL;
	convertData		= true;
	forceConvertData	= false;
	dlSegmentSize		= 8192;

#ifdef PRIV_USERAGENT
	string agentTmp1	= "MediathekView data to sql - ";
	string agentTmp2a	= "versionscheck ";
	string agentTmp2b	= "downloader ";
	string agentTmp2c	= "listcheck ";
	string agentTmp3	= "(" + (string)g_progName + "/" + (string)PROGVERSION + ", curl/" + (string)LIBCURL_VERSION + ")";
	userAgentCheck		= agentTmp1 + agentTmp2a + agentTmp3;
	userAgentDownload	= agentTmp1 + agentTmp2b + agentTmp3;
	userAgentListCheck	= agentTmp1 + agentTmp2c + agentTmp3;
#else
	userAgentCheck		= "";
	userAgentDownload	= "";
	userAgentListCheck	= "";
#endif
}

CMV2Mysql::~CMV2Mysql()
{
	configFile.setModifiedFlag(true);
	saveSetup(configFileName, true);
	videoInfo.clear();
	if (csql != NULL)
		delete csql;
}

void CMV2Mysql::printHeader()
{
	printf("%s %s\n", g_progName, g_progVersion);
}

void CMV2Mysql::printCopyright()
{
	printf("%s\n", g_progCopyright);
}

int CMV2Mysql::loadSetup(string fname)
{
	int erg = 0;
	if (!configFile.loadConfig(fname.c_str()))
		/* file not exist */
		erg = 1;

	/* test mode */
	g_settings.testLabel		= configFile.getString("testLabel",            "_TEST");
	g_settings.testMode		= configFile.getBool  ("testMode",             true);

	/* database */
	g_settings.videoDbBaseName	= configFile.getString("videoDbBaseName",      "mediathek_1");
	g_settings.videoDb		= configFile.getString("videoDb",              g_settings.videoDbBaseName);
	g_settings.videoDbTmp1		= configFile.getString("videoDbTmp1",          g_settings.videoDbBaseName + "_tmp1");
	g_settings.videoDbTemplate	= configFile.getString("videoDbTemplate",      g_settings.videoDbBaseName + "_template");
	g_settings.videoDb_TableVideo	= configFile.getString("videoDb_TableVideo",   "video");
	g_settings.videoDb_TableInfo	= configFile.getString("videoDb_TableInfo",    "channelinfo");
	g_settings.videoDb_TableVersion	= configFile.getString("videoDb_TableVersion", "version");
	VIDEO_DB_TMP_1			= g_settings.videoDbTmp1;
	if (g_settings.testMode) {
		VIDEO_DB_TMP_1	+= g_settings.testLabel;
	}

	/* download server */
	loadDownloadServerSetup();

	/* password file */
	g_settings.passwordFile		= configFile.getString("passwordFile",         "pw_mariadb");

	/* server list */
	g_settings.serverListUrl	 = configFile.getString("serverListUrl",               "https://res.mediathekview.de/akt.xml");
	g_settings.serverListLastRefresh = (time_t)configFile.getInt64("serverListLastRefresh", 0);
	g_settings.serverListRefreshDays = configFile.getInt32("serverListRefreshDays",         7);

	if (erg)
		configFile.setModifiedFlag(true);
	return erg;
}

void CMV2Mysql::saveSetup(string fname, bool quiet/*=false*/)
{
	/* test mode */
	configFile.setString("testLabel",            g_settings.testLabel);
	configFile.setBool  ("testMode",             g_settings.testMode);

	/* database */
	configFile.setString("videoDbBaseName",      g_settings.videoDbBaseName);
	configFile.setString("videoDb",              g_settings.videoDb);
	configFile.setString("videoDbTmp1",          g_settings.videoDbTmp1);
	configFile.setString("videoDbTemplate",      g_settings.videoDbTemplate);
	configFile.setString("videoDb_TableVideo",   g_settings.videoDb_TableVideo);
	configFile.setString("videoDb_TableInfo",    g_settings.videoDb_TableInfo);
	configFile.setString("videoDb_TableVersion", g_settings.videoDb_TableVersion);

	/* download server */
	saveDownloadServerSetup();

	/* password file */
	configFile.setString("passwordFile",         g_settings.passwordFile);

	/* server list */
	configFile.setString("serverListUrl",         g_settings.serverListUrl);
	configFile.setInt64 ("serverListLastRefresh", (int64_t)(g_settings.serverListLastRefresh));
	configFile.setInt32 ("serverListRefreshDays", g_settings.serverListRefreshDays);

	if (configFile.getModifiedFlag())
		configFile.saveConfig(fname.c_str(), '=', quiet);
}

void CMV2Mysql::loadDownloadServerSetup()
{
	char cfg_key[256];
	int count					= configFile.getInt32("downloadServerCount", 1);
	g_settings.downloadServerCount			= max(count, 1);
	g_settings.downloadServerCount			= min(count, MAX_DL_SERVER_COUNT);
	count						= configFile.getInt32("lastDownloadServer", 1);
	g_settings.lastDownloadServer			= max(count, 1);
	g_settings.lastDownloadServer			= min(count, g_settings.downloadServerCount);
	g_settings.lastDownloadTime			= (time_t)configFile.getInt64("lastDownloadTime", 0);
	g_settings.downloadServerConnectFailsMax	= configFile.getInt32("downloadServerConnectFailsMax", 3);
	for (int i = 1; i <= g_settings.downloadServerCount; i++) {
		memset(cfg_key, 0, sizeof(cfg_key));
		snprintf(cfg_key, sizeof(cfg_key), "downloadServer_%02d", i);
		g_settings.downloadServer[i] = configFile.getString(cfg_key, "-");
		memset(cfg_key, 0, sizeof(cfg_key));
		snprintf(cfg_key, sizeof(cfg_key), "downloadServerConnectFail_%02d", i);
		g_settings.downloadServerConnectFail[i] = configFile.getInt32(cfg_key, 0);
	}
}

void CMV2Mysql::saveDownloadServerSetup()
{
	char cfg_key[256];
	configFile.setInt32 ("downloadServerCount",           g_settings.downloadServerCount);
	configFile.setInt32 ("lastDownloadServer",            g_settings.lastDownloadServer);
	configFile.setInt64 ("lastDownloadTime",              (int64_t)(g_settings.lastDownloadTime));
	configFile.setInt32 ("downloadServerConnectFailsMax", g_settings.downloadServerConnectFailsMax);
	for (int i = 1; i <= g_settings.downloadServerCount; i++) {
		memset(cfg_key, 0, sizeof(cfg_key));
		snprintf(cfg_key, sizeof(cfg_key), "downloadServer_%02d", i);
		configFile.setString(cfg_key, g_settings.downloadServer[i]);
		memset(cfg_key, 0, sizeof(cfg_key));
		snprintf(cfg_key, sizeof(cfg_key), "downloadServerConnectFail_%02d", i);
		configFile.setInt32(cfg_key, g_settings.downloadServerConnectFail[i]);
	}
}

void CMV2Mysql::printHelp()
{
	printHeader();
	printCopyright();
	printf("  -e | --epoch xxx	 => Use not older entrys than 'xxx' days\n");
	printf("			    (default all data)\n");
	printf("  -f | --force-convert	 => Data also convert, when\n");
	printf("			    movie list is up-to-date.\n");
	printf("  -c | --cron-mode xxx	 => 'xxx' = time in minutes. Specifies the period during\n");
	printf("			    which no new version check is performed\n");
	printf("			    after the last download.\n");
	printf("  -C | --cron-mode-echo	 => Output message during --cron-mode to the log\n");
	printf("			    (Default: no output)\n");
	printf("  -n | --no-indexes	 => Don't create indexes for database\n");
	printf("       --update		 => Create new config file and\n");
	printf("			    new template database, then exit.\n");
	printf("       --download-only	 => Download only (Don't convert\n");
	printf("			    to sql database).\n");
	printf("       --load-serverlist => Load new serverlist and exit.\n");

	printf("\n");
	printf("  -d | --debug-print	 => Print debug info\n");
	printf("  -v | --version	 => Display versions info and exit\n");
	printf("  -h | --help		 => Display this help screen and exit\n");
}

int CMV2Mysql::run(int argc, char *argv[])
{
	/* Initialization random number generator */
	srand((uint32_t)time(0));

	/* set name for configFileName */
	string arg0         = (string)argv[0];
	string path0        = getPathName(arg0);
	path0               = getRealPath(path0);
	configFileName      = path0 + "/" + getBaseName(arg0) + ".conf";
	templateDBFile      = path0 + "/sql/"+ "template.sql";
	workDir             = path0 + "/dl/work";
	CFileHelpers cfh;
	if (!cfh.createDir(workDir, 0755)) {
		printf("Error: create dir %s\n", workDir.c_str());
		myExit(1);
	}

	int loadSettingsErg = loadSetup(configFileName);

	if (loadSettingsErg) {
		configFile.setModifiedFlag(true);
		saveSetup(configFileName);
	}

	/* set name for passwordFile */
	if ((g_settings.passwordFile)[0] == '/')
		g_passwordFile = g_settings.passwordFile;
	else
		g_passwordFile = path0 + "/" + g_settings.passwordFile;

	csql = new CSql();
	multiQuery = csql->multiQuery;

	int noParam       = 0;
	int requiredParam = 1;
//	int optionalParam = 2;
	static struct option long_options[] = {
		{"epoch",		requiredParam, NULL, 'e'},
		{"force-convert",	noParam,       NULL, 'f'},
		{"cron-mode",		requiredParam, NULL, 'c'},
		{"cron-mode-echo",	noParam,       NULL, 'C'},
		{"no-indexes",		noParam,       NULL, 'n'},
		{"update",		noParam,       NULL, '1'},
		{"download-only",	noParam,       NULL, '2'},
		{"load-serverlist",	noParam,       NULL, '3'},
		{"debug-print",		noParam,       NULL, 'd'},
		{"version",		noParam,       NULL, 'v'},
		{"help",		noParam,       NULL, 'h'},
		{NULL,			0,             NULL,  0 }
	};
	int c, opt;
	while ((opt = getopt_long(argc, argv, "e:fc:Cn123dvh?", long_options, &c)) >= 0) {
		switch (opt) {
			case 'e':
				/* >=0 and <=24800 */
				epoch = max(min(atoi(optarg), 24800), 0);
				break;
			case 'f':
				forceConvertData = true;
				break;
			case 'c':
				/* >=10 min. and <=600 min. */
				cronMode = max(min(atoi(optarg), 600), 10);
				break;
			case 'C':
				cronModeEcho = true;
				break;
			case 'n':
				createIndexes = false;
				break;
			case '1':
				configFile.setModifiedFlag(true);
				unlink(configFileName.c_str());
				saveSetup(configFileName);
				csql->connectMysql();
				csql->createTemplateDB(templateDBFile);
				return 0;
			case '2':
				downloadOnly = true;
				break;
			case '3':
				loadServerlist = true;
				break;
			case 'd':
				g_debugPrint = true;
				break;
			case 'v':
				printHeader();
				printCopyright();
				return 0;
			case 'h':
			case '?':
				printHelp();
				return (opt == '?') ? -1 : 0;
			default:
				break;
		}
	}

	if (cronMode > 0) {
		if ((time(0) - g_settings.lastDownloadTime) < (cronMode*60)) {
			if (cronModeEcho) {
				printf("[%s] The last download is recent enough.\n", g_progName);

				char buf[256];
				memset(buf, 0, sizeof(buf));
				time_t tt = g_settings.lastDownloadTime;
				struct tm* xTime = localtime(&tt);
				strftime(buf, sizeof(buf)-1, "%d.%m.%Y %H:%M", xTime);
				printf("[%s] Time of  last download is %s\n", g_progName, buf);

				memset(buf, 0, sizeof(buf));
				tt = g_settings.lastDownloadTime + cronMode*60;
				xTime = localtime(&tt);
				strftime(buf, sizeof(buf)-1, "%d.%m.%Y %H:%M", xTime);
				printf("[%s] Next possible download is %s\n", g_progName, buf); fflush(stdout);
			}
			/* The last download is recent enough, exit. */
			return 0;
		}
	}

	if (loadServerlist || ((g_settings.serverListLastRefresh + g_settings.serverListRefreshDays*24*3600) < time(0))) {
		CServerlist* csl = new CServerlist(userAgentListCheck);
		csl->getServerList();
		delete csl;
		printf("[%s] update download server list...\n", g_progName);
		if (g_debugPrint) {
			if (g_settings.downloadServerCount > 0) {
				for (int i = 1; i <= g_settings.downloadServerCount; i++) {
					printf("[%s] download server found: %s\n", g_progName, (g_settings.downloadServer[i]).c_str());
				}
			}
		}
		if (g_settings.downloadServerCount < 1) {
			printf("[%s] no download server found ;-(\n", g_progName);
			return 1;
		}
		g_settings.serverListLastRefresh = time(0);
	}

	if (!getDownloadUrlList())
		return 1;
	if (downloadOnly || !convertData) {
		CLZMAdec* xzDec = new CLZMAdec();
		xzDec->decodeXZ(xzName, jsonDbName);
		delete xzDec;
		const char* msg = (downloadOnly) ? "download only" : "no changes";
		printf("[%s] %s, don't convert to sql database\n", g_progName, msg); fflush(stdout);
		return 0;
	}

	csql->connectMysql();
	csql->checkTemplateDB(templateDBFile);
	parseDB();

	return 0;
}

void CMV2Mysql::setDbFileNames(string xz)
{
	string path0   = getPathName(xz);
	path0          = getRealPath(path0);
	string file0   = getBaseName(xz);
	xzName         = path0 + "/" + file0;
	jsonDbName     = workDir + "/" + getFileName(defaultXZ);
}

long CMV2Mysql::getDbVersion(string file)
{
	char buf[128];
	memset(buf, 0, sizeof(buf));
	FILE* f = fopen(file.c_str(), "r");
	fread(buf, sizeof(buf)-1, 1, f);
	fclose(f);

	string str1 = (string)buf;
	string search = "\"Filmliste\":";
	size_t pos1 = str1.find(search);
	if (pos1 != string::npos) {
		str1 = str1.substr(pos1 + search.length());
		pos1 = str1.find("\"");
		size_t pos2 = str1.find("\"", pos1+1);
		str1 = str1.substr(pos1+1, pos2-pos1-1);

		/* 28.08.2017, 05:19 */
		return str2time("%d.%m.%Y, %H:%M", str1);
	}
	return -1;
}

bool CMV2Mysql::checkNumberList(vector<uint32_t>* numberList, uint32_t number)
{
	if (numberList->empty())
		return false;
	for (vector<uint32_t>::iterator it = numberList->begin(); it != numberList->end(); ++it) {
		if ((uint32_t)(it[0]) == number)
			return true;
	}
	return false;
}

bool CMV2Mysql::getDownloadUrlList()
{
	uint32_t randStart  = 1;
	uint32_t randEnd    = g_settings.downloadServerCount;
	uint32_t whileCount = 0;
	uint32_t randValue;
	vector<uint32_t> numberList;

	while (true) {
		randValue = (rand() % ((randEnd + 1) - randStart)) + randStart;
		if (!checkNumberList(&numberList, randValue))
			numberList.push_back(randValue);
		if (numberList.size() >= randEnd)
			break;

		/* fallback for random error */
		whileCount++;
		if (whileCount >= randEnd*100)
			break;
	}

	for (uint32_t i = 0; i < numberList.size(); i++) {
		if (g_settings.downloadServerConnectFail[numberList[i]] >= g_settings.downloadServerConnectFailsMax)
			continue;
		string dlServer = g_settings.downloadServer[numberList[i]];
		if (g_debugPrint)
			printf("[%s-debug] check %s", g_progName, dlServer.c_str());
		if (downloadDB(dlServer)) {
			g_settings.downloadServerConnectFail[numberList[i]] = 0;
			g_settings.lastDownloadServer = numberList[i];
			return true;
		}
		else {
			g_settings.downloadServerConnectFail[numberList[i]] += 1;
			if (g_debugPrint)
				printf(" ERROR\n");
		}
	}
	printf("[%s] No download server found. ;-(\n", g_progName);
	return false;
}

long CMV2Mysql::getVersionFromXZ(string xz_, string json_)
{
	char buf[dlSegmentSize];
	FILE* f = fopen(xzName.c_str(), "r");
	fread(buf, sizeof(buf), 1, f);
	fclose(f);
	f = fopen(xz_.c_str(), "w+");
	fwrite(buf, sizeof(buf), 1, f);
	fclose(f);

	CLZMAdec* xzDec = new CLZMAdec();
	xzDec->decodeXZ(xz_, json_, false);
	delete xzDec;
	return getDbVersion(json_);
}

bool CMV2Mysql::downloadDB(string url)
{
	if ((xzName.empty()) || (jsonDbName.empty())) {
		string xz = getPathName(workDir) + "/" + defaultXZ;
		setDbFileNames(xz);
	}

	bool versionOK    = true;
	bool toFile       = true;
	string tmpXzOld   = getPathName(workDir) + "/tmp-old.xz";
	string tmpXzNew   = getPathName(workDir) + "/tmp-new.xz";
	string tmpJsonOld = getPathName(workDir) + "/tmp-old.json";
	string tmpJsonNew = getPathName(workDir) + "/tmp-new.json";
	CCurl* curl       = new CCurl();
	int ret;

	if (file_exists(xzName.c_str())) {
		/* check version */
		long oldVersion = getVersionFromXZ(tmpXzOld, tmpJsonOld);
		string range_ = (string)"0-" + to_string(dlSegmentSize-1);
		const char* range = range_.c_str();
		ret = curl->CurlDownload(url, tmpXzNew, toFile, userAgentCheck, true, false, range, true);
		if (ret != 0) {
			delete curl;
			return false;
		}
		if (!g_debugPrint)
			printf("[%s] version check %s\n", g_progName, url.c_str());
		CLZMAdec* xzDec = new CLZMAdec();
		xzDec->decodeXZ(tmpXzNew, tmpJsonNew, false);
		long newVersion = getDbVersion(tmpJsonNew);
		delete xzDec;
	
		if ((oldVersion != -1) && (newVersion != -1)) {
			if (newVersion > oldVersion)
				versionOK = false;
		}
		else
			versionOK = false;
	}
	else
		versionOK = false;

	convertData = (forceConvertData) ? true : !versionOK;

	CFileHelpers cfh;
	cfh.removeDir(workDir.c_str());
	cfh.createDir(workDir, 0755);
	unlink(tmpXzOld.c_str());
	unlink(tmpXzNew.c_str());
	unlink(tmpJsonOld.c_str());
	unlink(tmpJsonNew.c_str());

	if (!versionOK) {
		const char* range = NULL;
		ret = curl->CurlDownload(url, xzName, toFile, userAgentDownload, true, false, range, true);
		if (ret != 0) {
			delete curl;
			return false;
		}
		if (g_debugPrint)
			printf("\n");
		printf("[%s] movie list has been changed\n", g_progName);
		printf("[%s] curl download %s\n", g_progName, url.c_str());
		g_settings.lastDownloadTime = time(0);
	}
	else {
		if (g_debugPrint)
			printf("\n");
		printf("[%s] movie list is up-to-date, don't download\n", g_progName);
	}

	/* get version */
	long newVersion = getVersionFromXZ(tmpXzNew, tmpJsonNew);
	unlink(tmpXzNew.c_str());
	unlink(tmpJsonNew.c_str());

	struct tm* versionTime = gmtime(&newVersion);
	char buf[256];
	memset(buf, 0, sizeof(buf));
	strftime(buf, sizeof(buf)-1, "%d.%m.%Y %H:%M", versionTime);
	printf("[%s] movie list version: %s\n", g_progName, buf); fflush(stdout);

	delete curl;
	return true;
}

bool CMV2Mysql::repairJsonData(string db, string& data)
{
	printf("[%s] repair json data...", g_progName); fflush(stdout);

	ifstream jsonData(db.c_str(), ifstream::binary);
	if (!jsonData.is_open()) {
		printf("\n[%s %s:%d] Failed to open json db %s\n", g_progName, __func__, __LINE__, db.c_str());
		return false;
	}

	jsonData.seekg(0, jsonData.end);
	int length    = jsonData.tellg();
	int lengthNew = length+3;
	jsonData.seekg(0, jsonData.beg);
	char* buffer = new char[lengthNew];
	if (buffer == NULL) {
		printf("\n[%s %s:%d] memory error\n", g_progName, __func__, __LINE__);
		jsonData.close();
		return false;
	}

	jsonData.read(&(buffer[1]), length);
	jsonData.close();
	buffer[0] = '[';
	buffer[lengthNew-2] = ']';
	buffer[lengthNew-1] = '\0';

/*
],"X" => ]},{"X"
*/
	const char* cRet1 = cstr_replace("],\"X\"", "]},{\"X\"", buffer);
	delete [] buffer;
	if (cRet1 == NULL) {
		printf("\n[%s %s:%d] cstr_replace error\n", g_progName, __func__, __LINE__);
		return false;
	}

/*
],"Filmliste" => ]},{"Filmliste"
*/
	const char* cRet2 = cstr_replace("],\"Filmliste\"", "]},{\"Filmliste\"", cRet1);
	delete [] cRet1;
	if (cRet2 == NULL) {
		printf("\n[%s %s:%d] cstr_replace error\n", g_progName, __func__, __LINE__);
		return false;
	}
	data = (string)cRet2;
	delete [] cRet2;

	/* save repaired data (file is not needed) */
	ofstream out((db + ".json").c_str(), ofstream::binary);
	out.write (data.c_str(), data.size());
	out.close();
	printf("done.\n"); fflush(stdout);

	return true;
}

bool CMV2Mysql::parseDB()
{
	/* extract movie list */
	CLZMAdec* xzDec = new CLZMAdec();
	xzDec->decodeXZ(xzName, jsonDbName);
	delete xzDec;

	string jData;
	if (!repairJsonData(jsonDbName, jData))
		return false;

	printf("[%s] parse json db & write temporary database...", g_progName); fflush(stdout);

	string errMsg = "";
	Json::Value root;
	bool ok = parseJsonFromString(jData, &root, &errMsg);
	if (!ok) {
		printf("\nFailed to parse JSON\n");
		printf("[%s:%d] %s\n", __func__, __LINE__, errMsg.c_str());
		return false;
	}

	string cName = "";
	string tName = "";
	int cCount = 0;
	uint32_t entrys = 0;
	TVideoInfoEntry videoInfoEntry;
	videoInfoEntry.lastest = INT_MIN;
	videoInfoEntry.oldest = INT_MAX;
	time_t nowTime = time(NULL);
	struct timeval t1;
	double nowDTms;
	string vMultiQuery = "";
	string vQuery = "";
	uint32_t writeLen = 0;
	bool writeStart = true;
	uint32_t maxWriteLen = 1048576-4096;  /* 1MB */
	uint32_t skipUrl = 0;

	csql->createVideoDbFromTemplate(VIDEO_DB_TMP_1);
	csql->setUsedDatabase(VIDEO_DB_TMP_1);

	string sqlBuff = "";
	if (multiQuery)
		csql->setServerMultiStatementsOff();

	gettimeofday(&t1, NULL);
	nowDTms = (double)t1.tv_sec*1000ULL + ((double)t1.tv_usec)/1000ULL;
	if (g_debugPrint) {
		printf("\e[?25l"); /* cursor off */
		printf("\n");
	}

	for (unsigned int i = 0; i < root.size(); ++i) {
		Json::Value data;
		if (i == 0) {		/* head 1 */
			data = root[i].get("Filmliste", "");
			string tmp  = data[1].asString();
			g_mvDate    = str2time("%d.%m.%Y, %H:%M", tmp);
			g_mvVersion = data[3].asString();
		}
		else if (i == 1) {	/* head 2 */
			data = root[i].get("Filmliste", "");
		}
		else {			/* data   */
			data = root[i].get("X", "");
			TVideoEntry videoEntry;
			videoEntry.channel		= data[0].asString();
			if ((videoEntry.channel != "") && (videoEntry.channel != cName)) {
				if (cName != "") {
					videoInfo.push_back(videoInfoEntry);
				}
				cName = videoEntry.channel;
				cCount = 0;
				videoInfoEntry.lastest = INT_MIN;
				videoInfoEntry.oldest = INT_MAX;
			}

			videoEntry.theme		= data[1].asString();
			if (videoEntry.theme != "") {
				tName = videoEntry.theme;
			}
			else
				 videoEntry.theme	= tName;

			videoEntry.title		= data[2].asString();
			videoEntry.duration		= duration2time(data[5].asString());
			videoEntry.size_mb		= atoi(data[6].asCString());
			videoEntry.description		= data[7].asString();
			videoEntry.url			= data[8].asString();
			videoEntry.website		= data[9].asString();
			videoEntry.subtitle		= data[10].asString();
			videoEntry.url_rtmp		= convertUrl(videoEntry.url, data[11].asString());
			videoEntry.url_small		= convertUrl(videoEntry.url, data[12].asString());
			videoEntry.url_rtmp_small	= convertUrl(videoEntry.url, data[13].asString());
			videoEntry.url_hd		= convertUrl(videoEntry.url, data[14].asString());
			videoEntry.url_rtmp_hd		= convertUrl(videoEntry.url, data[15].asString());

			if ((videoEntry.url.empty())            &&
			    (videoEntry.url_rtmp.empty())       &&
			    (videoEntry.url_small.empty())      &&
			    (videoEntry.url_rtmp_small.empty()) &&
			    (videoEntry.url_hd.empty())         &&
			    (videoEntry.url_rtmp_hd.empty())) {
				skipUrl++;
				continue;
			}

			videoEntry.date_unix		= atoi((data[16].asCString()));
			if ((videoEntry.date_unix == 0) && (data[3].asString() != "") && (data[4].asString() != "")) {
				videoEntry.date_unix = str2time("%d.%m.%Y %H:%M:%S", data[3].asString() + " " + data[4].asString());
			}
			if ((videoEntry.date_unix > 0) && (epoch > 0)) {
				time_t maxDiff = (24*3600) * epoch; /* Not older than 'epoch' days (default all data) */
				if (videoEntry.date_unix < (nowTime - maxDiff))
					continue;
			}

			videoEntry.url_history		= data[17].asString();
			videoEntry.geo			= data[18].asString();
			videoEntry.new_entry		= ((data[19].asString() == "true") || (data[19].asString() == "TRUE")) ? true : false;

			videoEntry.channel		= cName;
			cCount++;
			videoInfoEntry.channel		= cName;
			videoInfoEntry.count		= cCount;
			videoInfoEntry.lastest		= max(videoEntry.date_unix, videoInfoEntry.lastest);
			if (videoEntry.date_unix != 0)
				videoInfoEntry.oldest	= min(videoEntry.date_unix, videoInfoEntry.oldest);

			entrys++;
			if (g_debugPrint) {
				if ((entrys % 32) == 0)
					printf("[%s-debug] Processed entries: %6d, skip (no url) %d\r", g_progName, entrys, skipUrl);
				if ((entrys % 32*8) == 0)
					fflush(stdout);
			}
			vQuery = csql->createVideoTableQuery(entrys, writeStart, &videoEntry);
			writeStart = false;

			if ((writeLen + vQuery.length()) >= maxWriteLen) {
				sqlBuff += ";\n";
				csql->executeSingleQueryString(sqlBuff);
				vQuery = csql->createVideoTableQuery(entrys, true, &videoEntry);
				sqlBuff = "";
				writeLen = 0;
			}
			sqlBuff += vQuery;
			writeLen = sqlBuff.length();
		}
	}

	if (g_debugPrint)
		printf("[%s-debug] Processed entries: %6d, skip (no url) %d\r", g_progName, entrys, skipUrl); fflush(stdout);

	if (!sqlBuff.empty()) {
		csql->executeSingleQueryString(sqlBuff);
		sqlBuff.clear();
	}
	if (multiQuery)
		csql->setServerMultiStatementsOn();

	if (g_debugPrint) {
		printf("\e[?25h"); /* cursor on */
		printf("\n");
	}

	videoInfo.push_back(videoInfoEntry);
	string itq = csql->createInfoTableQuery(&videoInfo, entrys);
	csql->executeMultiQueryString(itq);

	if (!g_debugPrint)
		printf("\n");

	if (entrys < 1000) {
		printf("\n[%s] Video list too small (%d entrys), no transfer to the database.\n", g_progName, entrys); fflush(stdout);
		return false;
	}

	csql->renameDB();
	if (createIndexes)
		csql->createIndex();

	if (skipUrl > 0)
		printf("[%s] skiped entrys (no url) %d\n", g_progName, skipUrl);
	string days_s = (epoch > 0) ? to_string(epoch) + " days" : "all data";
	printf("[%s] all tasks done (%u (%s) / %u entrys)\n", g_progName, entrys, days_s.c_str(), (uint32_t)(root.size()-2));
	gettimeofday(&t1, NULL);
	double workDTms = (double)t1.tv_sec*1000ULL + ((double)t1.tv_usec)/1000ULL;
	double workDTus = (double)t1.tv_sec*1000000ULL + ((double)t1.tv_usec);
	int32_t workTime = (int32_t)((workDTms - nowDTms) / 1000);
	double entryTime_us = (workDTus - nowDTms*1000) / entrys;
	printf("[%s] duration: %d sec (%.03f msec/entry)\n", g_progName, workTime, entryTime_us/1000);
	fflush(stdout);

	return true;
}

string CMV2Mysql::convertUrl(string url1, string url2)
{
	/* format url_small / url_rtmp_small etc:
		55|xxx.yyy
		 |    |
		 |    ----------  replace string
		 ---------------  replace pos in videoEntry.url (url1) */

	string ret = "";
	size_t pos = url2.find_first_of("|");
	if (pos != string::npos) {
		int pos1 = atoi(url2.substr(0, pos).c_str());
		ret = url1.substr(0, pos1) + url2.substr(pos+1);
	}
	else
		ret = url2;

	return ret;
}

const char* mySEMID = PROGNAME "_SEMID";
sem_t* mySemHandle = NULL;

void myExit(int val)
{
	/* Remove semaphore */
	if (mySemHandle != NULL)
		sem_close(mySemHandle);
	if (sem_unlink(mySEMID))
		perror(mySEMID);

	/* exit program */
	exit(val);
}

int main(int argc, char *argv[])
{
	g_mainInstance = NULL;
	/* Create semaphore to correctly identify
	 * the program to prevent multiple instances. */
	mySemHandle = sem_open(mySEMID, O_CREAT|O_EXCL);
	if (mySemHandle == SEM_FAILED) {
		if (errno == EEXIST)
			printf("[%s] An instance of '%s' is already running, this exits.\n", PROGNAME, PROGNAME);
		else
			perror(mySEMID);
		return 1;
	}

	/* main prog */
	g_mainInstance = new CMV2Mysql();
	int ret = g_mainInstance->run(argc, argv);
	delete g_mainInstance;

	/* Remove semaphore */
	if (mySemHandle != NULL)
		sem_close(mySemHandle);
	if (sem_unlink(mySEMID))
		perror(mySEMID);

	return ret;
}
