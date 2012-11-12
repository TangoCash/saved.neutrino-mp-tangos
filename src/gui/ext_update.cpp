/*
	update with settings - Neutrino-GUI

	Copyright (C) 2001 Steffen Hehn 'McClean'
	and some other guys
	Homepage: http://dbox.cyberphoria.org/

	Copyright (C) 2012 M. Liebmann (micha-bbg)

	License: GPL

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Library General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Library General Public License for more details.

	You should have received a copy of the GNU Library General Public
	License along with this library; if not, write to the
	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
	Boston, MA  02110-1301, USA.
*/

//#define UPDATE_DEBUG_TIMER
//#define UPDATE_DEBUG

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gui/update.h>
#include <gui/ext_update.h>

#include <global.h>
#include <neutrino.h>
#include <neutrino_menue.h>
#include <gui/widget/messagebox.h>
#include <gui/widget/hintbox.h>
#include <system/flashtool.h>

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fnmatch.h>
#include <fstream>
#include <sys/utsname.h>

CHintBox * hintBox = 0;

CExtUpdate::CExtUpdate()
{
	imgFilename     = "";
	mtdRamError     = "";
	mtdNumber       = -1;
	MTDBufSize      = 0xFFFF;
	MTDBuf          = new char[MTDBufSize];
	backupList      = CONFIGDIR "/settingsupdate.conf";
	defaultBackup   = CONFIGDIR;

	fUpdate         = NULL;
	updateLogBuf[0] = '\0';
	fLogEnabled     = 1;
	fLogfile        = "/tmp/update.log";
	mountPkt 	= "/tmp/image_mount";
	FileHelpers 	= NULL;
	copyList.clear();
	blackList.clear();
	deleteList.clear();
}

CExtUpdate::~CExtUpdate()
{
	if (MTDBuf != NULL)
		delete[] MTDBuf;
	if(FileHelpers)
		delete[] FileHelpers;
	copyList.clear();
	blackList.clear();
	deleteList.clear();
}

CExtUpdate* CExtUpdate::getInstance()
{
	static CExtUpdate* ExtUpdate = NULL;
	if(!ExtUpdate)
		ExtUpdate = new CExtUpdate();
	return ExtUpdate;
}

bool CExtUpdate::ErrorReset(bool modus, const std::string & msg1, const std::string & msg2)
{
	char buf[1024];

	if (modus & RESET_UNLOAD) {
		umount(mountPkt.c_str());
		my_system("rmmod", mtdramDriver.c_str());
	}
	if (modus & RESET_FD1)
		close(fd1);
	if (modus & RESET_FD2)
		close(fd2);
	if (modus & RESET_F1)
		fclose(f1);

	if (msg2 == "")
		snprintf(buf, sizeof(buf), "%s\n", msg1.c_str());
	else 
		snprintf(buf, sizeof(buf), "%s %s\n", msg1.c_str(), msg2.c_str());

	if ((msg1 != "") || (msg2 != "")) {
		mtdRamError = buf;
		WRITE_UPDATE_LOG("ERROR: %s", buf);
		printf(mtdRamError.c_str());
	}
	if(hintBox)
		hintBox->hide();
	sync();
	return false;
}

bool CExtUpdate::applySettings(const std::string & filename, int mode)
{
	if(!FileHelpers)
		FileHelpers = new CFileHelpers();

	if (mode == MODE_EXPERT)
		imgFilename = (std::string)g_settings.update_dir + "/" + FILESYSTEM_ENCODING_TO_UTF8_STRING(filename);
	else
		imgFilename = FILESYSTEM_ENCODING_TO_UTF8_STRING(filename);

	DBG_TIMER_START()
	bool ret = applySettings();
	DBG_TIMER_STOP("Image editing")
	if (!ret) {
		if (mtdRamError != "")
			DisplayErrorMessage(mtdRamError.c_str());
	}
	else {
		if (mode == MODE_EXPERT) {
			if ((mtdNumber < 3) || (mtdNumber > 4)) {
				const char *err = "invalid mtdNumber\n";
				printf(err);
				DisplayErrorMessage(err);
				WRITE_UPDATE_LOG("ERROR: %s", err);
				return false;
			}
		}
		ShowMsgUTF(LOCALE_MESSAGEBOX_INFO, g_Locale->getText(LOCALE_FLASHUPDATE_UPDATE_WITH_SETTINGS_SUCCESSFULLY), CMessageBox::mbrOk, CMessageBox::mbOk, NEUTRINO_ICON_INFO);
		WRITE_UPDATE_LOG("\n");
		WRITE_UPDATE_LOG("##### Settings taken. #####\n");
		if (mode == MODE_EXPERT)
			CFlashExpert::getInstance()->writemtd(filename, mtdNumber);
	}
	return ret;
}

bool CExtUpdate::isMtdramLoad()
{
	bool ret = false;
	FILE* f = fopen("/proc/modules", "r");
	if (f) {
		char buf[256] = "";
		while(fgets(buf, sizeof(buf), f) != NULL) {
			if (strstr(buf, "mtdram") != NULL) {
				ret = true;
				break;
			}
		}
		fclose(f);
	}
	return ret;
}

bool CExtUpdate::applySettings()
{
	if(!hintBox)
		hintBox = new CHintBox(LOCALE_MESSAGEBOX_INFO, g_Locale->getText(LOCALE_FLASHUPDATE_UPDATE_WITH_SETTINGS_PROCESSED));
	hintBox->paint();
	mtdRamError = "";
	std::string osrelease = "";
	mtdramDriver = ""; // driver path
	char buf1[256] = "";
	char buf2[256] = "";

	CMTDInfo * mtdInfo = CMTDInfo::getInstance();
	std::string mtdFilename = mtdInfo->findMTDsystem(); // /dev/mtdX
	int mtdSize = mtdInfo->getMTDSize(mtdFilename);
	int mtdEraseSize = mtdInfo->getMTDEraseSize(mtdFilename);
	mtdNumber = mtdInfo->findMTDNumber(mtdFilename);

	// get osrelease
	struct utsname uts_info;
	if( uname(&uts_info) == 0 ) {
		osrelease = uts_info.release;
		size_t pos = osrelease.find_first_of(" "); 
		if (pos != std::string::npos) 
			osrelease = osrelease.substr(0, pos); 
	}
	else
		return ErrorReset(0, "error no kernel info");

	// check if mtdram driver is already loaded
	if (!isMtdramLoad()) {
		// check if exist mtdram driver
		snprintf(buf1, sizeof(buf1), "/lib/modules/%s/mtdram.ko", osrelease.c_str());
		mtdramDriver = buf1;
		if ( !file_exists(mtdramDriver.c_str()) )
			return ErrorReset(0, "no mtdram driver available");
		// load mtdram driver
		snprintf(buf1, sizeof(buf1), "total_size=%d", mtdSize/1024);
		snprintf(buf2, sizeof(buf2), "erase_size=%d", mtdEraseSize/1024);
		my_system("insmod", mtdramDriver.c_str(), buf1, buf2);
		// check if mtdram driver is now loaded
		if (!isMtdramLoad())
			return ErrorReset(0, "error load mtdram driver");
	}
	else {
		DBG_MSG("mtdram driver is already loaded");
	}

	// find mtdram device
	std::string mtdRamFilename = "", mtdBlockFileName = "";
	int mtdRamSize, mtdRamEraseSize, mtdRamNr = 0;
	f1 = fopen("/proc/mtd", "r");
	if(!f1)
		return ErrorReset(RESET_UNLOAD, "cannot read /proc/mtd");
	fgets(buf1, sizeof(buf1), f1);
	while(!feof(f1)) {
		if(fgets(buf1, sizeof(buf1), f1)!=NULL) {
			char dummy[50] = "";
			sscanf(buf1, "mtd%1d: %8x %8x \"%48s\"\n", &mtdRamNr, &mtdRamSize, &mtdRamEraseSize, dummy);
			if (strstr(buf1, "mtdram test device") != NULL) {
				sprintf(buf1, "/dev/mtd%d", mtdRamNr);
				mtdRamFilename = buf1;
				sprintf(buf1, "/dev/mtdblock%d", mtdRamNr);
				mtdBlockFileName = buf1;
				break;
			}
		}
	}
	fclose(f1);

	if (mtdRamFilename == "")
		return ErrorReset(RESET_UNLOAD, "no mtdram test device found");
	else {
		// check mtdRamSize / mtdRamEraseSize
		if ((mtdRamSize != mtdSize) || (mtdRamEraseSize != mtdEraseSize)) {
			snprintf(buf2, sizeof(buf2), "error MTDSize(%08x/%08x) or MTDEraseSize(%08x/%08x)\n", mtdSize, mtdRamSize, mtdEraseSize, mtdRamEraseSize);
			return ErrorReset(RESET_UNLOAD, buf2);
		}
	}

	// copy image to mtdblock
	if (MTDBuf == NULL)
		return ErrorReset(RESET_UNLOAD, "malloc error");
	fd1 = open(imgFilename.c_str(), O_RDONLY);
	if (fd1 < 0)
		return ErrorReset(RESET_UNLOAD, "cannot read image file: " + imgFilename);
	long filesize = lseek(fd1, 0, SEEK_END);
	lseek(fd1, 0, SEEK_SET);
	if(filesize == 0)
		return ErrorReset(RESET_UNLOAD | RESET_FD1, "image filesize is 0");
	if(filesize > mtdSize)
		return ErrorReset(RESET_UNLOAD | RESET_FD1, "image filesize too large");
	fd2 = -1;
	int tmpCount = 0;
	while (fd2 < 0) {
		fd2 = open(mtdBlockFileName.c_str(), O_WRONLY);
		tmpCount++;
		if (tmpCount > 3)
			break;
		sleep(1);
	}
	if (fd2 < 0)
		return ErrorReset(RESET_UNLOAD | RESET_FD1, "cannot open mtdBlock");
	long fsize = filesize;
	long block;
	while(fsize > 0) {
		block = fsize;
		if(block > (long)MTDBufSize)
			block = MTDBufSize;
		read(fd1, MTDBuf, block);
		write(fd2, MTDBuf, block);
		fsize -= block;
	}
	close(fd1);
	close(fd2);

	FileHelpers->createDir(mountPkt.c_str(), 0755);
	int res = mount(mtdBlockFileName.c_str(), mountPkt.c_str(), "jffs2", 0, NULL);
	if (res)
		return ErrorReset(RESET_UNLOAD, "mount error");

	if (!readBackupList(mountPkt))
		return ErrorReset(0, "error readBackupList");

	res = umount(mountPkt.c_str());
	if (res)
		return ErrorReset(RESET_UNLOAD, "unmount error");

	unlink(imgFilename.c_str());

	// copy mtdblock to image
	if (MTDBuf == NULL)
		return ErrorReset(RESET_UNLOAD, "malloc error");
	fd1 = open(mtdBlockFileName.c_str(), O_RDONLY);
	if (fd1 < 0)
		return ErrorReset(RESET_UNLOAD, "cannot read mtdBlock");
	fsize = mtdRamSize;
	fd2 = open(imgFilename.c_str(), O_WRONLY | O_CREAT);
	if (fd2 < 0)
		return ErrorReset(RESET_UNLOAD | RESET_FD1, "cannot open image file: ", imgFilename);
	while(fsize > 0) {
		block = fsize;
		if(block > (long)MTDBufSize)
			block = MTDBufSize;
		read(fd1, MTDBuf, block);
		write(fd2, MTDBuf, block);
		fsize -= block;
	}
	lseek(fd2, 0, SEEK_SET);
	long fsizeDst = lseek(fd2, 0, SEEK_END);
	close(fd1);
	close(fd2);
	// check image file size
	if (mtdRamSize != fsizeDst) {
		unlink(imgFilename.c_str());
		return ErrorReset(0, "error file size: ", imgFilename);
	}

	// unload mtdramDriver only
	ErrorReset(RESET_UNLOAD);

	if(hintBox)
		hintBox->hide();

	sync();
	return true;
}

std::string Wildcard = "";

int fileSelect(const struct dirent *entry)
{
	if ((strcmp(entry->d_name, ".") == 0) || (strcmp(entry->d_name, "..") == 0))
		return 0;
	else
		if ((Wildcard != "") && (fnmatch(Wildcard.c_str(), entry->d_name, FNM_FILE_NAME)))
			return 0;
		else
			return 1;

}

bool CExtUpdate::copyFileList(const std::string & fileList, const std::string & dstPath)
{
	Wildcard = "";
	struct dirent **namelist;
	std::string fList = fileList, dst;
	static struct stat FileInfo;

	size_t pos = fileList.find_last_of("/");
	fList = fileList.substr(0, pos);
	Wildcard = fileList.substr(pos+1);

	int n = scandir(fList.c_str(), &namelist, fileSelect, 0);
	if (n > 0) {
		dst = dstPath + fList;
		FileHelpers->createDir(dst.c_str(), 0755);
		while (n--) {
			std::string dName = namelist[n]->d_name;
			if (lstat((fList+"/"+dName).c_str(), &FileInfo) != -1) {
				if (S_ISLNK(FileInfo.st_mode)) {
					char buf[PATH_MAX];
					int len = readlink((fList+"/"+dName).c_str(), buf, sizeof(buf)-1);
					if (len != -1) {
						buf[len] = '\0';
						WRITE_UPDATE_LOG("symlink: %s => %s\n", (dst+"/"+dName).c_str(), buf);
						symlink(buf, (dst+"/"+dName).c_str());
					}
				}
				else
					if (S_ISREG(FileInfo.st_mode)) {
						WRITE_UPDATE_LOG("copy %s => %s\n", (fList+"/"+dName).c_str(), (dst+"/"+dName).c_str());
						std::string save = (isBlacklistEntry(fList+"/"+dName)) ? ".save" : "";
						if (!FileHelpers->copyFile((fList+"/"+dName).c_str(), (dst + "/" + dName + save).c_str(), FileInfo.st_mode & 0x0FFF))
							return ErrorReset(0, "copyFile error");
					}
			}
			free(namelist[n]);
		}
		free(namelist);
	}

	return true;
}

bool CExtUpdate::deleteFileList(const std::string & fileList)
{
	Wildcard = "";
	struct dirent **namelist;
	std::string fList = fileList;
	static struct stat FileInfo;

	size_t pos = fileList.find_last_of("/");
	fList = fileList.substr(0, pos);
	Wildcard = fileList.substr(pos+1);

	int n = scandir(fList.c_str(), &namelist, fileSelect, 0);
	if (n > 0) {
		while (n--) {
			std::string dName = namelist[n]->d_name;
			if (lstat((fList+"/"+dName).c_str(), &FileInfo) != -1) {
				if (S_ISDIR(FileInfo.st_mode)) {
					// Directory
					WRITE_UPDATE_LOG("delete directory: %s\n", (fList+"/"+dName).c_str());
					FileHelpers->removeDir((fList+"/"+dName).c_str());
				}
				else if (S_ISREG(FileInfo.st_mode)) {
					// File
					WRITE_UPDATE_LOG("delete file: %s\n", (fList+"/"+dName).c_str());
					unlink((fList+"/"+dName).c_str());
				}
			}
			free(namelist[n]);
		}
		free(namelist);
	}
	return true;
}

bool CExtUpdate::findConfigEntry(std::string & line, std::string find)
{
	if (line.find("#:" + find + "=") == 0) {
		size_t pos = line.find_first_of('=');
		line = line.substr(pos+1);
		line = trim(line);
		return true;
	}
	return false;
}
	
bool CExtUpdate::readConfig(const std::string & line)
{
	std::string tmp1 = line;
	if (findConfigEntry(tmp1, "Log")) {
		if (tmp1 != "")
			fLogEnabled = atoi(tmp1.c_str());
		return true;
	}
	tmp1 = line;
	if (findConfigEntry(tmp1, "LogFile")) {
		if (tmp1 != "")
			fLogfile = tmp1;
		return true;
	}

	return true;
}

bool CExtUpdate::isBlacklistEntry(const std::string & file)
{
	for(vector<std::string>::iterator it = blackList.begin(); it != blackList.end(); ++it) {
		if (*it == file) {
			DBG_MSG("BlacklistEntry %s\n", (*it).c_str());
			WRITE_UPDATE_LOG("BlacklistEntry: %s\n", (*it).c_str());
			return true;
		}
	}
	return false;
}

bool CExtUpdate::checkSpecialFolders(std::string line, bool copy)
{
	if ((line == "/") || (line == "/*") || (line == "/*.*") || (line.find("/dev") == 0) || (line.find("/proc") == 0) || 
			(line.find("/sys") == 0) || (line.find("/mnt") == 0) || (line.find("/tmp") == 0)) {
		char buf[PATH_MAX];
		neutrino_locale_t msg = (copy) ? LOCALE_FLASHUPDATE_UPDATE_WITH_SETTINGS_SKIPPED : LOCALE_FLASHUPDATE_UPDATE_WITH_SETTINGS_DEL_SKIPPED;
		snprintf(buf, sizeof(buf), g_Locale->getText(msg), line.c_str());
		WRITE_UPDATE_LOG("%s%s", buf, "\n");
		ShowMsgUTF(LOCALE_MESSAGEBOX_INFO, buf, CMessageBox::mbrOk, CMessageBox::mbOk, NEUTRINO_ICON_INFO);
		return true;
	}
	return false;
}

bool CExtUpdate::readBackupList(const std::string & dstPath)
{
	char buf[PATH_MAX];
	static struct stat FileInfo;
	
	f1 = fopen(backupList.c_str(), "r");
	if (f1 == NULL) {
		f1 = fopen(backupList.c_str(), "w");
		if (f1 != NULL) {
			char tmp1[1024];
			snprintf(tmp1, sizeof(tmp1), "Log=%d\nLogFile=%s\n\n%s\n\n", fLogEnabled, fLogfile.c_str(), defaultBackup.c_str());
			fwrite(tmp1, 1, strlen(tmp1), f1);
			fclose(f1);
		}
		else
			return ErrorReset(0, "cannot create missing backuplist file: " + backupList);
	}

	f1 = fopen(backupList.c_str(), "r");
	if (f1 == NULL)
		return ErrorReset(0, "cannot read backuplist file: " + backupList);
	fpos_t fz;
	fseek(f1, 0, SEEK_END);
	fgetpos(f1, &fz);
	fseek(f1, 0, SEEK_SET);
	if (fz.__pos == 0)
		return ErrorReset(RESET_F1, "backuplist filesize is 0");
	size_t pos;
	std::string line;

	// read blacklist and config vars
	copyList.clear();
	blackList.clear();
	deleteList.clear();
	while(fgets(buf, sizeof(buf), f1) != NULL) {
		std::string tmpLine;
		line = buf;
		line = trim(line);
		// ignore comments
		if (line.find_first_of("#") == 0) {
			// config vars
			if (line.find_first_of(":") == 1) {
				if (line.length() > 1)
					readConfig(line);
			}
			continue;
		}
		pos = line.find_first_of("#");
		if (pos != std::string::npos) {
			line = line.substr(0, pos);
			line = trim(line);
		}
		// find blackList entry
		if (line.find_first_of("-") == 0) {
			tmpLine = line.substr(1);
			if ((tmpLine.length() > 1) && (lstat(tmpLine.c_str(), &FileInfo) != -1)) {
				if (S_ISREG(FileInfo.st_mode))
					blackList.push_back(tmpLine);
			}
		}
		// find deleteList entry
		else if (line.find_first_of("~") == 0) {
			tmpLine = line.substr(1);
			if (checkSpecialFolders(tmpLine, false))
				continue;
			tmpLine = dstPath + tmpLine;
			if (line.length() > 2)
				deleteList.push_back(tmpLine);
		}
		// find copyList entry
		else {
			tmpLine = (line.find_first_of("+") == 0) ? line.substr(1) : line; // '+' add entry = default
			if (checkSpecialFolders(tmpLine, true))
				continue;
			if (tmpLine.length() > 1)
				copyList.push_back(tmpLine);
		}
	}
	fclose(f1);

	// read copyList
	vector<std::string>::iterator it;
	for(it = copyList.begin(); it != copyList.end(); ++it) {
		line = *it;
		line = trim(line);
		// remove '/' from line end
		size_t len = line.length();
		pos = line.find_last_of("/");
		if (len == pos+1)
			line = line.substr(0, pos);
		std::string dst = dstPath + line;
		if ((line.find("*") != std::string::npos) || (line.find("?") != std::string::npos)) {
			// Wildcards
			DBG_MSG("Wildcards: %s\n", dst.c_str());
			WRITE_UPDATE_LOG("\n");
			WRITE_UPDATE_LOG("--------------------\n");
			WRITE_UPDATE_LOG("Wildcards: %s\n", dst.c_str());
			copyFileList(line, dstPath);
		}
		else {
			if (lstat(line.c_str(), &FileInfo) != -1) {
				if (S_ISREG(FileInfo.st_mode)) {
					// one file only
					pos = dst.find_last_of("/");
					std::string dir = dst.substr(0, pos);
					FileHelpers->createDir(dir.c_str(), 0755);
					DBG_MSG("file: %s => %s\n", line.c_str(), dst.c_str());
					WRITE_UPDATE_LOG("\n");
					WRITE_UPDATE_LOG("file: %s => %s\n", line.c_str(), dst.c_str());
					WRITE_UPDATE_LOG("--------------------\n");
					std::string save = (isBlacklistEntry(line)) ? ".save" : "";
					if (!FileHelpers->copyFile(line.c_str(), (dst + save).c_str(), FileInfo.st_mode & 0x0FFF))
						return ErrorReset(0, "copyFile error");
				}
				else if (S_ISDIR(FileInfo.st_mode)) {
					// directory
					DBG_MSG("directory: %s => %s\n", line.c_str(), dst.c_str());
					WRITE_UPDATE_LOG("\n");
					WRITE_UPDATE_LOG("directory: %s => %s\n", line.c_str(), dst.c_str());
					WRITE_UPDATE_LOG("--------------------\n");
					FileHelpers->copyDir(line.c_str(), dst.c_str(), true);
				}
			}
		
		}
	}

	// read DeleteList
	for(it = deleteList.begin(); it != deleteList.end(); ++it) {
		line = *it;
		if (lstat(line.c_str(), &FileInfo) != -1) {
			if ((line.find("*") != std::string::npos) || (line.find("?") != std::string::npos)) {
				// Wildcards
				WRITE_UPDATE_LOG("delete file list: %s\n", line.c_str());
				deleteFileList(line.c_str());
			}
			else if (S_ISREG(FileInfo.st_mode)) {
				// File
				WRITE_UPDATE_LOG("delete file: %s\n", line.c_str());
				unlink(line.c_str());
			}
			else if (S_ISDIR(FileInfo.st_mode)){
				// Directory
				WRITE_UPDATE_LOG("delete directory: %s\n", line.c_str());
				FileHelpers->removeDir(line.c_str());
			}
		}
	}
	sync();
	return true;
}

void CExtUpdate::updateLog(const char *buf)
{
	if ((!fUpdate) && (fLogEnabled))
		fUpdate = fopen(fLogfile.c_str(), "a");
	if (fUpdate) {
		fwrite(buf, 1, strlen(buf), fUpdate);
		fclose(fUpdate);
		fUpdate = NULL;
	}
}
