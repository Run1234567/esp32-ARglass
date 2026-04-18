# -*- coding: utf-8 -*-
import os

# Configuration
input_file = "components/tts_app/esp_tts_voice_data_xiaole.dat"
output_file = "components/tts_app/voice_data.c"

def convert():
    if not os.path.exists(input_file):
        print(f"Error: Cannot find {input_file}")
        return

    print("Reading binary file...")
    with open(input_file, "rb") as f:
        data = f.read()

    print(f"Converting {len(data)} bytes to C array...")
    with open(output_file, "w") as f:
        f.write('// Auto-generated voice data. Do not edit.\n')
        f.write('#include <stdint.h>\n\n')
        f.write('const uint8_t esp_tts_voice_data_xiaole_dat_start[] = {\n')
        
        # Write 12 bytes per line
        for i in range(0, len(data), 12):
            chunk = data[i:i+12]
            hex_data = ", ".join([f"0x{b:02x}" for b in chunk])
            f.write(f"    {hex_data},\n")
            
        f.write("};\n")
        print(f"Success! Generated {output_file}")

if __name__ == "__main__":
    convert()