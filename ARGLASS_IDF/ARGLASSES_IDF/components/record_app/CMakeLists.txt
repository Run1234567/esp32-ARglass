idf_component_register(SRCS "record_app.c"
                       INCLUDE_DIRS "."
                       # ���߱���������ģ����Ҫʹ�� SD������Ƶ �Լ� �ٷ�����ͷ�ײ�����
                       REQUIRES sd_card_app audio_app esp32-camera my_uart)