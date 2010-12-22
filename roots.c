/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <limits.h>

#include "mtdutils/mtdutils.h"
#include "mtdutils/mounts.h"
#include "mmcutils/mmcutils.h"
#include "minzip/Zip.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "common.h"

#include "extendedcommands.h"

/* Canonical pointers.
xxx may just want to use enums
 */
static const char g_mtd_device[] = "@\0g_mtd_device";
static const char g_mmc_device[] = "@\0g_mmc_device";
static const char g_raw[] = "@\0g_raw";
static const char g_package_file[] = "@\0g_package_file";
static int check_need=0; //We don't need to check in pre_menu

static RootInfo g_roots[] = {
    { "BOOT:", g_mtd_device, NULL, "boot", NULL, g_raw, NULL },
    { "CACHE:", CACHE_DEVICE, NULL, "cache", "/cache", CACHE_FILESYSTEM, CACHE_FILESYSTEM_OPTIONS },
    { "DATA:", DATA_DEVICE, NULL, "userdata", "/data", DATA_FILESYSTEM, DATA_FILESYSTEM_OPTIONS },
	{ "SYSTEM:", SYSTEM_DEVICE, NULL, "system", "/system", SYSTEM_FILESYSTEM, SYSTEM_FILESYSTEM_OPTIONS },
#ifdef HAS_DATADATA
    { "DATADATA:", DATADATA_DEVICE, NULL, "datadata", "/datadata", DATADATA_FILESYSTEM, DATADATA_FILESYSTEM_OPTIONS },
#endif
    { "PACKAGE:", NULL, NULL, NULL, NULL, g_package_file, NULL },
    { "RECOVERY:", g_mtd_device, NULL, "recovery", "/", g_raw, NULL },
    { "SDCARD:", SDCARD_DEVICE_PRIMARY, SDCARD_DEVICE_SECONDARY, NULL, "/sdcard", "vfat", NULL },
    { "SDEXT:", SDEXT_DEVICE, NULL, NULL, "/sd-ext", SDEXT_FILESYSTEM, NULL },
    { "MBM:", g_mtd_device, NULL, "mbm", NULL, g_raw, NULL },
    { "TMP:", NULL, NULL, NULL, "/tmp", NULL, NULL },
    { "EFS:", EFS_DEVICE, NULL, "efs", "/efs", EFS_FILESYSTEM, EFS_FILESYSTEM_OPTIONS },
};
#define NUM_ROOTS (sizeof(g_roots) / sizeof(g_roots[0]))

// TODO: for SDCARD:, try /dev/block/mmcblk0 if mmcblk0p1 fails

static int
internal_root_mounted(const RootInfo *info)
{
    if (info->mount_point == NULL) {
        return -1;
    }
//xxx if TMP: (or similar) just say "yes"

    /* See if this root is already mounted.
     */
    int ret = scan_mounted_volumes();
    if (ret < 0) {
        return ret;
    }
    const MountedVolume *volume;
    volume = find_mounted_volume_by_mount_point(info->mount_point);
    if (volume != NULL) {
        /* It's already mounted.
         */
        return 0;
    }
    return -1;
}


void recheck() {
	check_need=1;
}
	

//We are actually not using it, but mkfs and e2fsck look for it.
void create_mtab() {
	FILE* f=fopen("/etc/mtab","r");
	if ( f == NULL ) {
		if (chdir("/etc")) {
			chdir("/");
			unlink("/etc"); //Success only if it's a symlink(file)
			dirUnlinkHierarchy("/etc"); //Then it should be an empty dir
			mkdir("/etc",0777);
		}
		f=fopen("/etc/mtab","w");
		if ( f != NULL ) fclose(f);
	}
}

int create_mknods(int n) {
	static mknods_ready=0;
	if (!mknods_ready) {
		char mknod_cmd[PATH_MAX];
		int i;
		int err=0;
		for (i=0; i<n; ++i) {
			sprintf(mknod_cmd,"/dev/loop%d",i);
			FILE* f=fopen(mknod_cmd,"r");
			if ( f != NULL ) {
				fclose(f);
				continue;
			}
			sprintf(mknod_cmd,"/bin/mknod /dev/loop%d b 7 %d",i,i);
			if ( __system(mknod_cmd) ) {
				fprintf(stderr,"Can't create mknods:%s\n",strerror(errno));
				err=1;
			}
		}
		if (!err) mknods_ready=1;
	}
	if (mknods_ready) return 0;
	else return 1;
}


//First check should be called when ALL the Filesystems are unmounted except SDCARD
static void check_fs() {
	LOGI("Checking FS types\n");
	int i;
	static int already=0; //We will allocate the memory for options only in the first run
	RootInfo *info;
       for (i = 1 ;i < 4 ;i++){
          info = &g_roots[i];
          if (!already) info->filesystem_options=malloc(256*sizeof(char)); //1B will be enough for every FS. This way we don't have to allocate new MEM at every recheck.			  
          if ( internal_root_mounted(info) < 0 ) {
			  if(chdir(info->mount_point)){
				mkdir(info->mount_point, 0755);  // in case it doesn't already exist
			  } else chdir("/");
			  if ( !strncmp(info->device,"/sdcard/",8) ) {
				  char check_cmd[PATH_MAX];
				  create_mtab();
				  sprintf(check_cmd,"/sbin/e2fsck -fyc %s",info->device);
				  __system(check_cmd);
				  ensure_root_path_unmounted(info->name); //Just in case e2fsck mounted it
				  strcpy(info->filesystem,"ext4");
				  const char options[] = "loop,nodev,nosuid,noatime,nodiratime,data=ordered";
				  strcpy(info->filesystem_options,options);
			  } else {
				  info->filesystem=calloc(5,sizeof(char));
				  if ( !mount(info->device, info->mount_point, "rfs", MS_NODEV | MS_NOSUID, "codepage=utf8,xattr,check=no")) {
					  strcpy(info->filesystem,"rfs");
					  const char options[] = "nodev,nosuid,codepage=utf8,xattr,check=no";
					  strcpy(info->filesystem_options,options);
				  }
				  else {
					  char check_cmd[PATH_MAX];
					  create_mtab();
					  sprintf(check_cmd,"/sbin/e2fsck -fyc %s",info->device);
					  __system(check_cmd);
					  ensure_root_path_unmounted(info->name); //Just in case e2fsck mounted it
					  if ( !mount(info->device, info->mount_point, "ext2", MS_NODEV | MS_NOSUID | MS_NOATIME | MS_NODIRATIME, NULL)) {
					  strcpy(info->filesystem,"ext2");
					  const char options[] = "nodev,nosuid,noatime,nodiratime";
					  strcpy(info->filesystem_options,options);
					  }
					  else if ( !mount(info->device, info->mount_point, "ext4", MS_NODEV | MS_NOSUID | MS_NOATIME | MS_NODIRATIME, NULL)) {
						  strcpy(info->filesystem,"ext4");
						  const char options[] = "nodev,nosuid,noatime,nodiratime,data=ordered";
						  strcpy(info->filesystem_options,options);
					  }
					  else {
						  strcpy(info->filesystem,"auto");
						  info->filesystem_options = NULL; //safety
					  }
				  }
			  }
		  }
		  ui_print("."); //A little progress
       }
       ui_print("\n");
       already=1;

       //We don't need them already mounted
       ensure_root_path_unmounted("SYSTEM:");
       ensure_root_path_unmounted("DATA:");
 }

const RootInfo *
get_root_info_for_path(const char *root_path)
{
    const char *c;

    /* Find the first colon.
     */
    c = root_path;
    while (*c != '\0' && *c != ':') {
        c++;
    }
    if (*c == '\0') {
        return NULL;
    }

	if (check_need) {
		check_need=0;
		check_fs();
	}
    
    size_t len = c - root_path + 1;
    size_t i;
    for (i = 0; i < NUM_ROOTS; i++) {
        RootInfo *info = &g_roots[i];
        if (strncmp(info->name, root_path, len) == 0) {
            return info;
        }
    }
    return NULL;
}

static const ZipArchive *g_package = NULL;
static char *g_package_path = NULL;

int
register_package_root(const ZipArchive *package, const char *package_path)
{
    if (package != NULL) {
        package_path = strdup(package_path);
        if (package_path == NULL) {
            return -1;
        }
        g_package_path = (char *)package_path;
    } else {
        free(g_package_path);
        g_package_path = NULL;
    }
    g_package = package;
    return 0;
}

int
is_package_root_path(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    return info != NULL && info->filesystem == g_package_file;
}

const char *
translate_package_root_path(const char *root_path,
        char *out_buf, size_t out_buf_len, const ZipArchive **out_package)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->filesystem != g_package_file) {
        return NULL;
    }

    /* Strip the package root off of the path.
     */
    size_t root_len = strlen(info->name);
    root_path += root_len;
    size_t root_path_len = strlen(root_path);

    if (out_buf_len < root_path_len + 1) {
        return NULL;
    }
    strcpy(out_buf, root_path);
    *out_package = g_package;
    return out_buf;
}

/* Takes a string like "SYSTEM:lib" and turns it into a string
 * like "/system/lib".  The translated path is put in out_buf,
 * and out_buf is returned if the translation succeeded.
 */
const char *
translate_root_path(const char *root_path, char *out_buf, size_t out_buf_len)
{
    if (out_buf_len < 1) {
        return NULL;
    }

    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->mount_point == NULL) {
        return NULL;
    }

    /* Find the relative part of the non-root part of the path.
     */
    root_path += strlen(info->name);  // strip off the "root:"
    while (*root_path != '\0' && *root_path == '/') {
        root_path++;
    }

    size_t mp_len = strlen(info->mount_point);
    size_t rp_len = strlen(root_path);
    if (mp_len + 1 + rp_len + 1 > out_buf_len) {
        return NULL;
    }

    /* Glue the mount point to the relative part of the path.
     */
    memcpy(out_buf, info->mount_point, mp_len);
    if (out_buf[mp_len - 1] != '/') out_buf[mp_len++] = '/';

    memcpy(out_buf + mp_len, root_path, rp_len);
    out_buf[mp_len + rp_len] = '\0';

    return out_buf;
}

int
is_root_path_mounted(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        return -1;
    }
    return internal_root_mounted(info) >= 0;
}

static int mount_internal(const char* device, const char* mount_point, const char* filesystem, const char* filesystem_options)
{
    if (strcmp(filesystem, "auto") != 0 && filesystem_options == NULL) {
        return mount(device, mount_point, filesystem, MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
    }
    else {
        char mount_cmd[PATH_MAX];
        const char* options = filesystem_options == NULL ? "noatime,nodiratime,nodev,nosuid" : filesystem_options;
        sprintf(mount_cmd, "mount -t %s -o%s %s %s", filesystem, options, device, mount_point);
        return __system(mount_cmd);
    }
}

int
ensure_root_path_mounted(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        return -1;
    }

    int ret = internal_root_mounted(info);
    if (ret >= 0) {
        /* It's already mounted.
         */
        return 0;
    }

    /* It's not mounted.
     */
    if (info->device == g_mtd_device) {
        if (info->partition_name == NULL) {
            return -1;
        }
//TODO: make the mtd stuff scan once when it needs to
        mtd_scan_partitions();
        const MtdPartition *partition;
        partition = mtd_find_partition_by_name(info->partition_name);
        if (partition == NULL) {
            return -1;
        }
        return mtd_mount_partition(partition, info->mount_point,
                info->filesystem, 0);
    }

    if (info->device == NULL || info->mount_point == NULL ||
        info->filesystem == NULL ||
        info->filesystem == g_raw ||
        info->filesystem == g_package_file) {
        return -1;
    }

    mkdir(info->mount_point, 0755);  // in case it doesn't already exist
    if (mount_internal(info->device, info->mount_point, info->filesystem, info->filesystem_options)) {
        if (info->device2 == NULL) {
            LOGE("Can't mount %s\n(%s)\n", info->device, strerror(errno));
            return -1;
        } else if (mount(info->device2, info->mount_point, info->filesystem,
                MS_NOATIME | MS_NODEV | MS_NODIRATIME, "")) {
            LOGE("Can't mount %s (or %s)\n(%s)\n",
                    info->device, info->device2, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int
ensure_root_path_unmounted(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL) {
        return -1;
    }
    if (info->mount_point == NULL) {
        /* This root can't be mounted, so by definition it isn't.
         */
        return 0;
    }
//xxx if TMP: (or similar) just return error

    /* See if this root is already mounted.
     */
    int ret = scan_mounted_volumes();
    if (ret < 0) {
        return ret;
    }
    const MountedVolume *volume;
    volume = find_mounted_volume_by_mount_point(info->mount_point);
    if (volume == NULL) {
        /* It's not mounted.
         */
        return 0;
    }

    return unmount_mounted_volume(volume);
}

const MtdPartition *
get_root_mtd_partition(const char *root_path)
{
    const RootInfo *info = get_root_info_for_path(root_path);
    if (info == NULL || info->device != g_mtd_device ||
            info->partition_name == NULL)
    {
#ifdef BOARD_HAS_MTD_CACHE
        if (strcmp(root_path, "CACHE:") != 0)
            return NULL;
#else
        return NULL;
#endif
    }
    mtd_scan_partitions();
    return mtd_find_partition_by_name(info->partition_name);
}

int
format_root_device(const char *root)
{
    /* Be a little safer here; require that "root" is just
     * a device with no relative path after it.
     */
    const char *c = root;
    while (*c != '\0' && *c != ':') {
        c++;
    }
    /*
    if (c[0] != ':' || c[1] != '\0') {
        LOGW("format_root_device: bad root name \"%s\"\n", root);
        return -1;
    }
    */

    const RootInfo *info = get_root_info_for_path(root);
    if (info == NULL || info->device == NULL) {
        LOGW("format_root_device: can't resolve \"%s\"\n", root);
        return -1;
    }

    if (info->mount_point != NULL) {
        /* Don't try to format a mounted device.
         */
        int ret = ensure_root_path_unmounted(root);
        if (ret < 0) {
            LOGW("format_root_device: can't unmount \"%s\"\n", root);
            return ret;
        }
    }

    //Handle MMC device types
    if(info->device == g_mmc_device) {
        mmc_scan_partitions();
        const MmcPartition *partition;
        partition = mmc_find_partition_by_name(info->partition_name);
        if (partition == NULL) {
            LOGE("format_root_device: can't find mmc partition \"%s\"\n",
                    info->partition_name);
            return -1;
        }
        if (!strcmp(info->filesystem, "ext3")) {
            if(mmc_format_ext3(partition))
                LOGE("\n\"%s\" wipe failed!\n", info->partition_name);
        }
    }

    else {
        pid_t pid = fork();
		 if (pid == 0) {
		     if (info->filesystem != NULL && strncmp(info->filesystem, "ext",3) == 0) {
	                char fst[10];
	                sprintf(fst,"-T %s",info->filesystem);
	                create_mtab();
		         LOGW("format: %s as %s\n", info->device, info->filesystem);
	                if (strncmp(info->filesystem, "ext4",4) == 0) {					
						///sbin/mke2fs -T ext4 -F -q -m 0 -b 4096 -O ^huge_file,extent /sdcard/cm6/data.img
                           char* args[] = {"/sbin/mke2fs", fst, "-F", "-q", "-m", "0", "-b", "4096", "-O", "^huge_file,extent", info->device, NULL};
	                   execv(args[0], args);
	                } else {
                           char* args[] = {"/sbin/mke2fs", fst, "-F", "-q", "-m", "0", "-b", "4096", info->device, NULL};
	                   execv(args[0], args);
	                }
	                LOGE("E:Can't run mke2fs format [%s]\n", strerror(errno));       
		     } 
		     else if (info->filesystem != NULL && strcmp(info->filesystem, "rfs")==0){
		          LOGW("format: %s as rfs\n", info->device);
	                 char* args[] = {"/sbin/stl.format", info->device, NULL};
	                 execv("/sbin/stl.format", args);
	                 fprintf(stderr, "E:Can't run STL format [%s]\n", strerror(errno));
	            }
	         else {
				 //We couldn't detect FS, so formatting as default RFS
				 LOGW("Fallback format: %s as rfs\n", info->device);
	                 char* args[] = {"/sbin/stl.format", info->device, NULL};
	                 execv("/sbin/stl.format", args);
	                 fprintf(stderr, "E:Can't run STL format [%s]\n", strerror(errno));
				}
	        _exit(-1);
		 }
		 
	        int status;
	
	        while (waitpid(pid, &status, WNOHANG) == 0) {
	            ui_print(".");
	            sleep(1);
	        }
	        ui_print("\n");
	
	        if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
	            LOGW("format_root_device: can't erase \"%s\"\n", root);
		    return -1;
	        }
		return 0;
		}
    
    if (info->mount_point != NULL && info->device == g_mtd_device) {
        /* Don't try to format a mounted device.
         */
        int ret = ensure_root_path_unmounted(root);
        if (ret < 0) {
            LOGW("format_root_device: can't unmount \"%s\"\n", root);
            return ret;
        }
    }

    /* Format the device.
     */
    if (info->device == g_mtd_device) {
        mtd_scan_partitions();
        const MtdPartition *partition;
        partition = mtd_find_partition_by_name(info->partition_name);
        if (partition == NULL) {
            LOGW("format_root_device: can't find mtd partition \"%s\"\n",
                    info->partition_name);
            return -1;
        }
        if (info->filesystem == g_raw || !strcmp(info->filesystem, "yaffs2")) {
            MtdWriteContext *write = mtd_write_partition(partition);
            if (write == NULL) {
                LOGW("format_root_device: can't open \"%s\"\n", root);
                return -1;
            } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
                LOGW("format_root_device: can't erase \"%s\"\n", root);
                mtd_write_close(write);
                return -1;
            } else if (mtd_write_close(write)) {
                LOGW("format_root_device: can't close \"%s\"\n", root);
                return -1;
            } else {
                return 0;
            }
        }
    }

    return format_non_mtd_device(root);
}
