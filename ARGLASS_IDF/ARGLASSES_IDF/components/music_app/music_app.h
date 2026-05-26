#ifndef MUSIC_APP_H
#define MUSIC_APP_H

#ifdef __cplusplus
extern "C" {
#endif

void start_music_player(const char *path);
void stop_music_player(void);
void pause_music_player(void);
void resume_music_player(void);
void seek_music_player(int sec);
void set_music_volume(int vol);

#ifdef __cplusplus
}
#endif

#endif // MUSIC_APP_H
