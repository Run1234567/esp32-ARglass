#include "ui_globals.h"
#include "ui_pitch_screen.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <math.h>

lv_obj_t * ui_pitch_screen;
static lv_obj_t * label_note;
static lv_obj_t * label_hz;
static lv_obj_t * visualizer_bar;

void ui_pitch_screen_init(void) {
    ui_pitch_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_pitch_screen, lv_color_hex(0x0A0212), 0);
    lv_obj_set_style_bg_grad_color(ui_pitch_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(ui_pitch_screen, LV_GRAD_DIR_VER, 0);

    lv_obj_t * title = lv_label_create(ui_pitch_screen);
    lv_label_set_text(title, "音调检测仪");
    lv_obj_set_style_text_font(title, &my_font_cn_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xBC8CF2), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    label_note = lv_label_create(ui_pitch_screen);
    lv_label_set_text(label_note, "--");
    lv_obj_set_style_text_color(label_note, lv_color_hex(0xBDFF00), 0);
    lv_obj_set_style_text_font(label_note, &lv_font_montserrat_48, 0);
    lv_obj_align(label_note, LV_ALIGN_CENTER, 0, -20);

    label_hz = lv_label_create(ui_pitch_screen);
    lv_label_set_text(label_hz, "0.0 Hz");
    lv_obj_set_style_text_color(label_hz, lv_color_hex(0x8888AA), 0);
    lv_obj_set_style_text_font(label_hz, &my_font_cn_16, 0);
    lv_obj_align(label_hz, LV_ALIGN_CENTER, 0, 30);

    visualizer_bar = lv_bar_create(ui_pitch_screen);
    lv_obj_set_size(visualizer_bar, 150, 4);
    lv_obj_align(visualizer_bar, LV_ALIGN_CENTER, 0, 55);
    lv_obj_set_style_bg_color(visualizer_bar, lv_color_hex(0x331144), 0);
    lv_obj_set_style_bg_color(visualizer_bar, lv_color_hex(0xBC8CF2), LV_PART_INDICATOR);
}

static const char* hz_to_note(float freq) {
    if (freq < 16.0f) return "--";
    const char* notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int midi = (int)roundf(12.0f * log2f(freq / 440.0f) + 69.0f);
    int note_idx = midi % 12;
    int octave = (midi / 12) - 1;
    static char buf[16];
    snprintf(buf, sizeof(buf), "%s%d", notes[note_idx], octave);
    return buf;
}

void update_pitch_ui(float freq) {
    if (lvgl_port_lock(0)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f Hz", freq);
        lv_label_set_text(label_hz, buf);

        lv_label_set_text(label_note, hz_to_note(freq));

        lv_bar_set_value(visualizer_bar, (int)freq % 100, LV_ANIM_ON);

        lvgl_port_unlock();
    }
}
