#!/usr/bin/env python3
"""Authenticode-signs Windows programs in place, when a code-signing certificate is configured.

  scripts/windows/sign.py FILE...

The certificate comes from the environment (CI: the repository secrets WINDOWS_SIGNING_CERT_BASE64 /
WINDOWS_SIGNING_CERT_PASSWORD, decoded to a file by build.yml):
  SAFEHOUSE_WINDOWS_SIGN_PFX        a PKCS#12 file (.pfx / .p12) with the certificate and its key
  SAFEHOUSE_WINDOWS_SIGN_PASSWORD   its password
  SAFEHOUSE_WINDOWS_SIGN_TIMESTAMP  RFC 3161 timestamp server (default http://timestamp.digicert.com)
Without SAFEHOUSE_WINDOWS_SIGN_PFX nothing is signed and the script exits 0 (the packages are unsigned, as before).
Signing uses osslsigncode on Linux (apt install osslsigncode) or signtool.exe on Windows. Called by scripts/package.py
for the programs in the package and by scripts/windows/setup.nsi (!uninstfinalize / !finalize) for the uninstaller and
the setup. The password is only passed through the environment / a file, never on a command line other programs see.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

NAME = 'SafeHouse'
URL = 'https://github.com/FridiNaTor1/SafeHouse'


def configured():
    return bool(os.environ.get('SAFEHOUSE_WINDOWS_SIGN_PFX'))


def sign(files):
    pfx = os.environ.get('SAFEHOUSE_WINDOWS_SIGN_PFX', '')
    if not pfx:
        return 0
    if not pathlib.Path(pfx).is_file():
        sys.exit('sign.py: SAFEHOUSE_WINDOWS_SIGN_PFX is not a file: ' + pfx)
    password = os.environ.get('SAFEHOUSE_WINDOWS_SIGN_PASSWORD', '')
    timestamp = os.environ.get('SAFEHOUSE_WINDOWS_SIGN_TIMESTAMP', 'http://timestamp.digicert.com')
    osslsigncode = shutil.which('osslsigncode')
    signtool = shutil.which('signtool') or shutil.which('signtool.exe')
    if not osslsigncode and not signtool:
        sys.exit('sign.py: a certificate is configured but neither osslsigncode nor signtool was found')
    with tempfile.TemporaryDirectory() as temporary:
        secret = pathlib.Path(temporary) / 'password'
        secret.write_text(password)
        for file in map(pathlib.Path, files):
            if osslsigncode:
                signed = file.with_name(file.name + '.signed')
                signed.unlink(missing_ok=True)
                subprocess.run([osslsigncode, 'sign', '-pkcs12', pfx, '-readpass', str(secret), '-h', 'sha256',
                                '-n', NAME, '-i', URL, '-ts', timestamp, '-in', str(file), '-out', str(signed)], check=True)
                os.replace(signed, file)
            else:
                # (signtool takes the password only on its command line; CI signs on Linux with osslsigncode)
                subprocess.run([signtool, 'sign', '/f', pfx, '/p', password, '/fd', 'sha256', '/d', NAME, '/du', URL,
                                '/tr', timestamp, '/td', 'sha256', str(file)], check=True)
            print('signed ' + str(file))
    return 0


if __name__ == '__main__':
    sys.exit(sign(sys.argv[1:]))
