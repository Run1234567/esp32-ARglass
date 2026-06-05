/**
 * @file music_app.h
 * @brief WAV 音乐播放器模块公共接口
 *
 * 本模块提供从 SD 卡播放 WAV 音频文件的功能。
 * 支持播放/暂停/停止/跳转/音量控制。
 * 音频格式: 16kHz / 16-bit / 单声道 PCM WAV
 */

#ifndef MUSIC_APP_H
#define MUSIC_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开始播放 WAV 文件
 * @param path WAV 文件完整路径 (如 "/sdcard/音乐/歌曲1.wav")
 *
 * 如果当前正在播放，会先停止再开始新文件。
 * 创建独立的 FreeRTOS 任务 (Core 1) 进行播放。
 */
void start_music_player(const char *path);

/**
 * @brief 停止播放
 * 立即停止当前播放并删除播放任务
 */
void stop_music_player(void);

/**
 * @brief 暂停播放
 * 仅在 MUSIC_PLAYING 状态下有效
 */
void pause_music_player(void);

/**
 * @brief 恢复播放
 * 仅在 MUSIC_PAUSED 状态下有效
 */
void resume_music_player(void);

/**
 * @brief 跳转到指定时间
 * @param sec 目标位置 (秒)
 */
void seek_music_player(int sec);

/**
 * @brief 设置播放音量
 * @param vol 音量值 (0-100)
 */
void set_music_volume(int vol);

#ifdef __cplusplus
}
#endif

#endif // MUSIC_APP_H
