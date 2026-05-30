#ifndef TTS_APP_H
#define TTS_APP_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "freertos/semphr.h"

// 3. ���ļ��������������а�����ͷ�ļ��� C �ļ�������������ڣ�
extern SemaphoreHandle_t speaker_mutex;
/**
 * @brief ��ʼ����ά˹ TTS ��������ϵͳ
 * ��������ʼ��ͬ��������Ƶ������������ SD ��������ѧģ�͵� PSRAM
 * ��Ҫ�� app_main ���ڵ��á�
 */
void init_tts_engine(void);

/**
 * @brief ������ϵͳ׷�����ı� (�첽������)
 * �����ַ����ı����������������أ��ɺ�̨�����Զ�����ת�������š�
 * @param new_text ��Ҫ������ UTF-8 �ַ���
 */
void jarvis_add_text(const char *new_text);

/**
 * @brief �ı�ת�������� (���ݾɰ����)
 * �ڲ����Զ��ض����첽�� jarvis_add_text����ֹԭ�д��뱨����
 * @param text ��Ҫ������ UTF-8 �ַ���
 */
void tts_speak(const char *text);

void tts_set_speed(int speed);
void stop_tts_reading(void);

extern volatile int global_tts_speed;
extern volatile bool is_reading_active;

#ifdef __cplusplus
}
#endif

#endif // TTS_APP_H