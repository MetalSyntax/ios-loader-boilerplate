#!/usr/bin/env python3
"""Sincroniza ux0_data/<slug>/ con ux0:data/<slug>/ de la PS Vita por FTP (VitaShell, puerto 1337).

- Sube solo lo que falta o cambió de tamaño.
- Protege saves/ (partidas del jugador) para no sobreescribir ni borrar nada.
- --logs: descarga logs/log_NNN.log y .psp2dmp desde la consola hacia logs/ local.

Uso: python3 -I tools/deploy_data.py [--ip 192.168.x.y] [--logs] [--dry-run]
"""
import ftplib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Auto-detectar slug e IP
slug = "ios_game"
vita_ip = "192.168.1.100"

cfg_file = os.path.join(ROOT, ".psvita-toolkit.json")
if os.path.exists(cfg_file):
    try:
        with open(cfg_file, "r", encoding="utf-8") as f:
            c = json.load(f)
            slug = c.get("slug") or slug
            vita_ip = c.get("vita_ip") or vita_ip
    except Exception:
        pass

cmake_file = os.path.join(ROOT, "CMakeLists.txt")
if os.path.exists(cmake_file):
    txt = open(cmake_file, errors="ignore").read()
    m_data = re.search(r'ux0:data/([^/]+)/?', txt)
    if m_data:
        slug = m_data.group(1)
    m_ip = re.search(r'set\(PSVITAIP\s+"([^"]+)"', txt)
    if m_ip:
        vita_ip = m_ip.group(1)

# Procesar argumentos CLI
dry_run = "--dry-run" in sys.argv
only_logs = "--logs" in sys.argv

if "--ip" in sys.argv:
    idx = sys.argv.index("--ip")
    if idx + 1 < len(sys.argv):
        vita_ip = sys.argv[idx + 1]

LOCAL = os.path.join(ROOT, "ux0_data", slug)
REMOTE = f"/ux0:/data/{slug}"
NO_PUSH = {"saves", "logs"}


def ls(ftp, path):
    out = {}
    try:
        ftp.cwd(path)
        lines = []
        ftp.retrlines("LIST", lines.append)
    except ftplib.error_perm:
        return None
    for l in lines:
        p = l.split(None, 8)
        if len(p) == 9 and p[8] not in (".", ".."):
            out[p[8]] = (l[0] == "d", int(p[4]))
    return out


def push(ftp, local, remote, dry, stats):
    have = ls(ftp, remote)
    if have is None:
        print("  mkdir", remote)
        dry or ftp.mkd(remote)
        have = {}
    if not os.path.exists(local):
        return
    for n in sorted(os.listdir(local)):
        if n.startswith("._") or n == ".DS_Store":
            continue
        lp, rp = os.path.join(local, n), f"{remote}/{n}"
        if remote == REMOTE and n in NO_PUSH:
            if n not in have:
                print("  mkdir", rp)
                dry or ftp.mkd(rp)
            continue
        if os.path.isdir(lp):
            push(ftp, lp, rp, dry, stats)
        else:
            sz = os.path.getsize(lp)
            if n in have and not have[n][0] and have[n][1] == sz:
                continue
            print(f"  put {n} ({sz // 1024} KB)")
            if not dry:
                with open(lp, "rb") as f:
                    ftp.storbinary(f"STOR {rp}", f, blocksize=65536)
            stats["uploaded"] += 1


def fetch_logs(ftp):
    local_logs = os.path.join(ROOT, "logs")
    os.makedirs(local_logs, exist_ok=True)
    # Descargar logs del directorio del juego
    remote_logs = f"{REMOTE}/logs"
    entries = ls(ftp, remote_logs) or {}
    for n, (is_dir, sz) in entries.items():
        if not is_dir and n.startswith("log_") and n.endswith(".log"):
            lp = os.path.join(local_logs, n)
            if not os.path.exists(lp) or os.path.getsize(lp) != sz:
                print(f"  get {remote_logs}/{n} -> logs/{n}")
                with open(lp, "wb") as f:
                    ftp.retrbinary(f"RETR {remote_logs}/{n}", f.write)
    # Descargar dumps de ux0:data/
    root_entries = ls(ftp, "/ux0:/data") or {}
    for n, (is_dir, sz) in root_entries.items():
        if not is_dir and (n.endswith(".psp2dmp") or n.startswith("psp2core-")):
            lp = os.path.join(local_logs, n)
            if not os.path.exists(lp):
                print(f"  get dump /ux0:/data/{n} -> logs/{n}")
                with open(lp, "wb") as f:
                    ftp.retrbinary(f"RETR /ux0:/data/{n}", f.write)


def main():
    print(f"[*] Conectando por FTP a PS Vita en {vita_ip}:1337...")
    try:
        ftp = ftplib.FTP()
        ftp.connect(vita_ip, 1337, timeout=10)
        ftp.login()
    except Exception as e:
        print(f"[-] Error conectando a {vita_ip}:1337 (asegúrate de que VitaShell tiene FTP activo): {e}")
        sys.exit(1)

    print("[+] Conexión FTP establecida.")
    if only_logs:
        print("[*] Descargando logs y crash dumps...")
        fetch_logs(ftp)
        print("[+] Listo. Revisa la carpeta logs/.")
    else:
        print(f"[*] Sincronizando {LOCAL} -> {REMOTE}...")
        stats = {"uploaded": 0}
        push(ftp, LOCAL, REMOTE, dry_run, stats)
        print(f"[+] Despliegue finalizado. Archivos subidos: {stats['uploaded']}.")

    ftp.quit()


if __name__ == "__main__":
    main()
