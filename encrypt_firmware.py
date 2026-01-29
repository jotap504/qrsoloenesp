import sys
import os
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend

def encrypt_file(input_file, output_file, key):
    # AES-256-CBC
    iv = os.urandom(16)
    cipher = Cipher(algorithms.AES(key), modes.CBC(iv), backend=default_backend())
    encryptor = cipher.encryptor()

    with open(input_file, 'rb') as f:
        plaintext = f.read()

    # Padding PKCS7
    pad_len = 16 - (len(plaintext) % 16)
    plaintext += bytes([pad_len] * pad_len)

    ciphertext = encryptor.update(plaintext) + encryptor.finalize()

    with open(output_file, 'wb') as f:
        f.write(iv) # First 16 bytes are IV
        f.write(ciphertext)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python encrypt_firmware.py <input.bin> <output.enc> <hex_key>")
        sys.exit(1)

    in_file = sys.argv[1]
    out_file = sys.argv[2]
    key_hex = sys.argv[3]
    
    key = bytes.fromhex(key_hex)
    if len(key) != 32:
        print("Error: Key must be 32 bytes (64 hex characters)")
        sys.exit(1)

    encrypt_file(in_file, out_file, key)
    print(f"File encrypted successfully: {out_file}")
