#ifndef MUSIC_APP_H
#define MUSIC_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动音乐播放器 (后台任务)
 * @param path SD卡中WAV音乐文件的绝对路径，例如 "/sdcard/music.wav"
 */
void start_music_player(const char *path);

#ifdef __cplusplus
}
#endif

#endif // MUSIC_APP_H