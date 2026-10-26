/* Win32 fs slice set-up: the two host folders behind the virtual Windows
 * paths (runtime/src/win32_fs.c). Start-up calls both before guest code runs. */
#ifndef WIN32_FS_H
#define WIN32_FS_H

/* C:\Game\: the folder that holds the game's .exe. Never written to. */
void w32_fs_set_game_dir(const char *host_dir);

/* Saves, settings and every write aimed at C:\Game\ go below this folder. */
void w32_fs_set_save_dir(const char *host_dir);

/* Create host directory path and its missing parents (mkdir -p). Returns 0,
 * or -1 with errno. */
int w32_fs_mkdir_p(const char *path);

#endif
