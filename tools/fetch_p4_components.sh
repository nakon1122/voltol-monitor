#!/bin/sh
# Fetch the KSDIY P4C5 panel/touch/PMIC drivers from the ESP component registry into lib/
# as PlatformIO libraries (they are plain esp_lcd / i2c_master drivers).
set -e
cd "$(dirname "$0")/.."
fetch() {  # namespace/name version libdir
  url=$(curl -s "https://components.espressif.com/api/components/$1" | python3 -c "
import json,sys
d=json.load(sys.stdin); want='$2'
vs=[v for v in d['versions'] if not want or v['version'].startswith(want)] or d['versions']
v=vs[0]; print(v.get('url') or v.get('download_url'), v['version'])")
  [ -n "$url" ] || { echo "no download for $1"; exit 1; }
  ver=${url#* }; url=${url%% *}
  base=$(echo "${1#*/}" | tr 'a-z' 'A-Z')
  maj=$(echo "$ver" | cut -d. -f1); min=$(echo "$ver" | cut -d. -f2); pat=$(echo "$ver" | cut -d. -f3 | cut -d~ -f1)
  vflags="\"-D${base}_VER_MAJOR=$maj\",\"-D${base}_VER_MINOR=$min\",\"-D${base}_VER_PATCH=${pat:-0}\","
  rm -rf "lib/$3" && mkdir -p "lib/$3"
  curl -sL "$url" -o /tmp/c.zip && unzip -q -o /tmp/c.zip -d "lib/$3"
  inc=include; [ -d "lib/$3/include" ] || inc=.
  cat > "lib/$3/library.json" <<J
{"name":"$3","version":"0.0.0","build":{"srcDir":".","includeDir":"$inc","srcFilter":["+<*.c>","+<src/*.c>","-<test*/>","-<examples/>"],"flags":[$vflags"-I.","-Iinclude","-Ipriv_include","-DCONFIG_ESP_LCD_TOUCH_MAX_POINTS=5","-DCONFIG_ESP_LCD_TOUCH_MAX_BUTTONS=1","-Wno-unused-variable"]}}
J
  echo "$1 -> lib/$3"; ls "lib/$3"
}
fetch kevincoooool/esp_lcd_st7102 "" ksdiy_st7102
fetch kevincoooool/esp_lcd_touch_st7123 "1.0" ksdiy_st7123
fetch kevincoooool/pmic_axp2101 "" ksdiy_axp2101
fetch espressif/esp_lcd_touch "" ksdiy_lcd_touch
