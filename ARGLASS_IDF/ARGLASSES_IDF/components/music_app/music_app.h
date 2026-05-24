#ifndef MUSIC_APP_H
#define MUSIC_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动音乐播放器 (后台任务)
 * @param path SD卡上WAV音频文件的绝对路径，如 "/sdcard/music.wav"
 */
void start_music_player(const char *path);

/**
 * @brief 停止音乐播放 (中途刹车)
 */
void stop_music_player(void);

#ifdef __cplusplus
}
#endif

#endif // MUSIC_APP_H