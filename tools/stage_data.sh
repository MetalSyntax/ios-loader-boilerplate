#!/bin/sh
# Prepara ux0_data/<slug>/ (= ux0:data/<slug>/ en la PS Vita) desde el app bundle (game_app).
# Imita el sandbox de iOS para que las rutas del juego se resuelvan directamente:
#   <AppName>.app/   bundle limpio (ejecutable + recursos), sin firmas de Apple ni basura
#   rebase.bin       tabla de rebase generada por tools/macho_prep.py
#   saves/           partidas (Documents/ de iOS); deploy nunca sobreescribe ni borra su contenido
#   logs/            log_001.log .. log_999.log rotativos escritos por el loader
#
# Uso: tools/stage_data.sh [ruta/a/App.app]

set -e
cd "$(dirname "$0")/.."

APP="${1:-game_app}"
if [ ! -d "$APP" ]; then
    echo "[-] Error: No se encuentra el bundle en $APP (¿falta crear el symlink game_app?)"
    exit 1
fi

# Detectar ejecutable e identificador desde Info.plist
EXEC_NAME=$(python3 -c "
import plistlib, sys
try:
    with open('$APP/Info.plist', 'rb') as f:
        pl = plistlib.load(f)
    print(pl.get('CFBundleExecutable', ''))
except Exception:
    print('')
")

if [ -z "$EXEC_NAME" ] || [ ! -f "$APP/$EXEC_NAME" ]; then
    # Fallback: buscar el binario ejecutable más grande en la raíz del .app
    EXEC_NAME=$(find "$APP" -maxdepth 1 -type f -perm +111 -exec ls -1S {} + 2>/dev/null | head -n 1 | xargs basename 2>/dev/null || echo "")
fi

if [ -z "$EXEC_NAME" ] || [ ! -f "$APP/$EXEC_NAME" ]; then
    echo "[-] Error: No se pudo identificar el ejecutable Mach-O en $APP"
    exit 1
fi

# Detectar slug
SLUG=$(python3 -c "
import json, re, os
slug = ''
if os.path.exists('.psvita-toolkit.json'):
    try:
        with open('.psvita-toolkit.json') as f:
            slug = json.load(f).get('slug', '')
    except Exception:
        pass
if not slug and os.path.exists('CMakeLists.txt'):
    m = re.search(r'ux0:data/([^/]+)/?', open('CMakeLists.txt').read())
    if m:
        slug = m.group(1)
print(slug or 'ios_game')
")

APP_DIR_NAME=$(basename "$APP")
if [ "$APP_DIR_NAME" = "game_app" ]; then
    # Usar el nombre real del .app al que apunta el symlink
    REAL_APP_PATH=$(readlink "$APP" || echo "$APP")
    APP_DIR_NAME=$(basename "$REAL_APP_PATH")
fi

OUT="ux0_data/$SLUG"
echo "[*] Preparando datos para '$SLUG' desde '$APP/$EXEC_NAME'..."
mkdir -p "$OUT/$APP_DIR_NAME" "$OUT/saves" "$OUT/logs" "docs"

# Copiar recursos del bundle excluyendo firmas y metadatos innecesarios
rsync -a --delete \
  --exclude '._*' \
  --exclude '.DS_Store' \
  --exclude '_CodeSignature' \
  --exclude 'SC_Info' \
  --exclude 'embedded.mobileprovision' \
  --exclude 'CodeResources' \
  "$APP/" "$OUT/$APP_DIR_NAME/"

# Generar rebase.bin y reporte de imports
echo "[*] Analizando Mach-O y generando rebase.bin..."
python3 -I tools/macho_prep.py "$OUT/$APP_DIR_NAME/$EXEC_NAME" "$OUT/rebase.bin" --report docs/macho_report.txt

echo "[+] Staging completado en $OUT/"
echo "    - $OUT/$APP_DIR_NAME/ ($EXEC_NAME y recursos)"
echo "    - $OUT/rebase.bin"
echo "    - $OUT/saves/ (directorio de guardado)"
echo "    - $OUT/logs/ (directorio de logs)"
echo "[+] Listo para desplegar con: python3 -I tools/deploy_data.py o 'psvita-toolkit deploy --data'"
