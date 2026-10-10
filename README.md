# AI-Lens — Tahap 1: Prototipe Pengalaman Pengguna (tanpa lensa)

Tujuan tahap ini: **menguji cara membaca jawaban AI di layar kecil** sebelum memikirkan lensa.
Tiga pertanyaan yang ingin dijawab:

1. **Berapa karakter yang nyaman dibaca** per baris dan per layar?
2. **Kapan teks sebaiknya muncul**: langsung per kata (streaming), sekaligus setelah selesai, atau setelah jeda?
3. **Bagaimana cara mengontrolnya**: tombol fisik, pindah halaman otomatis, kata demi kata (RSVP), atau teks berjalan?

## Isi paket

| File | Fungsi |
|---|---|
| `firmware/ai_lens_esp32/ai_lens_esp32.ino` | Program ESP32: terima teks lewat Bluetooth, tampilkan di OLED 128×64, tombol NEXT/PREV |
| `index.html` | Aplikasi HP (satu file): tanya AI, kirim ke layar, simulator layar, catat hasil uji, ekspor CSV |
| `README.md` | Panduan ini |

Aplikasi bisa langsung dipakai **tanpa ESP32**: ada simulator layar yang tata letaknya identik dengan layar asli (huruf per baris, baris per layar, dan pemenggalan kata sama persis).

---

## 1. Komponen

Harga perkiraan pasar online Indonesia, cek ulang sebelum beli.

| Komponen | Keterangan | Perkiraan |
|---|---|---|
| ESP32 DevKit V1 (ESP32-WROOM-32) | Pilih yang ada tombol BOOT | Rp60–100 rb |
| OLED 0,96" SSD1306 I2C 128×64 | 4 pin: GND, VCC, SCL, SDA | Rp30–50 rb |
| Tombol tactile 6×6 mm | 1 buah untuk PREV (NEXT pakai tombol BOOT) | Rp1–2 rb |
| Breadboard + kabel jumper | | Rp20–30 rb |
| Kabel USB data (micro-USB / USB-C sesuai board) | Harus kabel data, bukan hanya charger | – |
| Opsional: powerbank kecil | Supaya bisa dibawa/dipakai di kepala | – |

OLED 1,3" (SH1106) juga bisa: ubah konstruktor di bagian atas firmware.

## 2. Wiring

| OLED | ESP32 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

| Tombol | ESP32 |
|---|---|
| NEXT | Tombol **BOOT** bawaan board (GPIO 0), tidak perlu kabel |
| PREV | Tombol tactile antara **GPIO 4** dan **GND** |

Fungsi tombol:

| Tombol | Tekan singkat | Tahan (≥0,7 detik) |
|---|---|---|
| NEXT | Halaman berikut · (RSVP/berjalan: jeda/lanjut) | Minta HP mulai input suara |
| PREV | Halaman sebelumnya · (RSVP: mundur 10 kata · berjalan: ulang) | Kosongkan layar |

## 3. Upload firmware

1. Pasang **Arduino IDE 2.x**.
2. *File › Preferences › Additional boards manager URLs*, isi:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. *Tools › Board › Boards Manager*, pasang **esp32 by Espressif Systems** (versi 3.x atau 2.x).
4. *Tools › Manage Libraries*, pasang **U8g2** (oleh oliver).
5. Buka `ai_lens_esp32.ino`, lalu atur:
   - *Board*: **ESP32 Dev Module**
   - *Partition Scheme*: **Huge APP (3MB No OTA/1MB SPIFFS)** (Bluetooth butuh ruang program besar)
   - *Port*: port USB ESP32
6. Klik **Upload**. Jika macet di "Connecting...", tahan tombol BOOT sampai upload mulai.
7. Layar menampilkan **AI-Lens · Menunggu HP...**

**Tes tanpa HP:** buka *Serial Monitor* (115200 baud, line ending **Newline**), ketik:

```
MSG Halo, ini uji layar AI-Lens. Tekan tombol BOOT untuk halaman berikut.
CFG font=2
CFG mode=rsvp;wpm=250
```

## 4. Jalankan aplikasi HP

Web Bluetooth dan input suara **wajib dibuka lewat https://**. Cara termudah: GitHub Pages.

1. Di GitHub, buat repository baru (mis. `ai-lens`), upload `app/index.html`.
2. *Settings › Pages › Source: Deploy from a branch*, pilih `main` / root, simpan.
3. Buka `https://<username>.github.io/ai-lens/` di **Chrome Android**.
4. Opsional: menu Chrome › *Tambahkan ke layar utama*, supaya terasa seperti aplikasi.

Browser yang didukung:

| Perangkat | Browser |
|---|---|
| Android | Chrome (paling stabil) |
| PC/laptop | Chrome atau Edge |
| iPhone/iPad | Safari tidak mendukung Web Bluetooth. Pakai aplikasi **Bluefy**. |

Hubungkan: nyalakan Bluetooth dan Lokasi di HP, tekan **Hubungkan**, pilih **AI-Lens**.

### Sumber AI (tab Pengaturan)

| Pilihan | Keterangan |
|---|---|
| **Demo** | Jawaban contoh tanpa internet. Cukup untuk menguji tampilan dan kontrol. |
| **Claude (Anthropic)** | Isi API key dari console.anthropic.com. Model bawaan `claude-haiku-5-5` (cepat dan murah, cocok untuk layar kecil). |
| **Kompatibel OpenAI** | OpenAI, OpenRouter, atau server sendiri (llama.cpp / Ollama di Z800). Alamat server wajib https, mis. lewat Cloudflare Tunnel: `https://ai.domainanda.com/v1`. Ollama perlu `OLLAMA_ORIGINS=*`. |
| **Webhook (n8n)** | Aplikasi mengirim POST JSON `{question, system, max_words, history}`. Balas dengan `{"answer": "..."}` atau teks biasa. Webhook harus mengizinkan CORS: isi opsi *Allowed Origins (CORS)* di node Webhook n8n, atau tambahkan header `Access-Control-Allow-Origin: *` di node *Respond to Webhook*. |

API key disimpan **hanya di browser HP tersebut** (localStorage), tidak ikut ter-upload ke GitHub. Untuk dipakai banyak orang, lebih aman lewat webhook n8n supaya kunci tetap di server.

---

## 5. Protokol eksperimen

Lakukan di tempat terang dan redup, posisi layar tetap (mis. pada dudukan setinggi mata, jarak 30–40 cm). Idealnya 3–5 orang, masing-masing ±15 menit. Semua data tercatat di tab **Log** dan bisa diekspor ke Excel (CSV, pemisah `;`).

### Uji A — Berapa karakter yang nyaman

1. Tab **Tampilan**: cara tampil = Halaman, pindah otomatis = mati.
2. Untuk setiap ukuran huruf (**Kecil 21×6**, **Sedang 18×4**, **Besar 12×3**):
   kirim **Teks uji Pendek, Sedang, Panjang**. Baca sampai habis dengan tombol NEXT, lalu beri nilai 1–5 dan tanda (*Terlalu padat / Pas / Terlalu sedikit*).
3. Hasil yang dicari: ukuran dengan nilai tertinggi dan kecepatan baca (kata/menit) yang masih tinggi.

### Uji B — Kapan teks muncul

1. Pakai ukuran huruf terbaik dari Uji A.
2. Ajukan 3–5 pertanyaan nyata di setiap pilihan: **Langsung**, **Setelah selesai**, **Setelah jeda 1,5 s**. Coba juga indikator *Berpikir...* aktif dan mati.
3. Bandingkan nilai dan catatan *Muncul terlalu lama*. Kolom `token_pertama_s` dan `teks_tampil_s` di CSV menunjukkan berapa lama pengguna menunggu.

### Uji C — Cara kontrol

1. Bandingkan: **Halaman + tombol**, **Halaman + pindah otomatis**, **Kata demi kata (RSVP)**, **Berjalan**.
2. Untuk mode otomatis, coba kecepatan 150, 200, dan 250 kata/menit.
3. Catat mana yang paling sedikit membuat pengguna tertinggal atau harus mengulang.

### Cara membaca hasil

Di tab **Log › Ringkasan per setelan**, setelan diurutkan dari nilai rata-rata tertinggi. Setelan juara dari ketiga uji menjadi **spesifikasi tampilan untuk Tahap 2**, misalnya: "maksimal 18 huruf × 4 baris, teks muncul per kata, pindah halaman dengan tombol, jawaban maksimal 40 kata".

---

## 6. Referensi protokol Bluetooth

Layanan: Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`).
RX (HP → ESP32) `6E400002-…`, TX (ESP32 → HP, notify) `6E400003-…`.
Satu perintah per baris, diakhiri `\n`. Baris baru di dalam teks dikirim sebagai byte `0x1F`.

| Arah | Perintah | Arti |
|---|---|---|
| HP → ESP32 | `MSG <teks>` | Ganti isi layar |
| | `ADD <teks>` | Tambah teks di akhir (streaming) |
| | `CFG k=v;k=v` | `mode=page\|rsvp\|ticker`, `font=0\|1\|2`, `wpm=60–1000`, `auto=0\|1`, `bar=0\|1`, `contrast=0–255`, `rot=0\|2\|m\|mv` |
| | `NEXT` / `PREV` | Sama dengan tombol fisik |
| | `GOTO <n>` | Lompat ke halaman n |
| | `WAIT` | Tampilkan "Berpikir..." |
| | `CLEAR` | Kosongkan layar |
| | `PING` | Dibalas `PONG v1 128x64` |
| ESP32 → HP | `PAGE x/y` | Halaman sekarang |
| | `END` | Pengguna melewati halaman terakhir (dipakai menghitung waktu baca) |
| | `PAUSE 0/1` | Status jeda RSVP/berjalan |
| | `BTN ASK` / `BTN CLEAR` | Tombol ditahan |
| | `CFG OK` | Setelan diterapkan |

Protokol ini sengaja sederhana dan berbasis teks, sehingga perangkat Tahap 2 (layar mikro di kacamata, lalu lensa) cukup mengikuti protokol yang sama tanpa mengubah aplikasi.

## 7. Masalah umum

| Gejala | Solusi |
|---|---|
| AI-Lens tidak muncul saat Hubungkan | Nyalakan Bluetooth **dan Lokasi**; tutup aplikasi lain yang terhubung ke ESP32; tekan tombol EN/RESET di board. |
| Upload gagal "Sketch too big" | Partition Scheme belum diganti ke **Huge APP**. |
| Layar gelap | Cek VCC ke 3V3 dan SDA/SCL tidak tertukar. Modul 1,3" pakai konstruktor SH1106. |
| Huruf bergeser 2 piksel di OLED 1,3" | Itu ciri SH1106 bila dipakai konstruktor SSD1306: ganti konstruktor. |
| Error CORS / "Failed to fetch" | Server AI belum mengizinkan akses dari browser, atau alamatnya http (bukan https). |
| Teks terkirim sebagian | Turunkan *Ukuran paket Bluetooth* ke 20 di tab Pengaturan. |

## 8. Catatan keselamatan

Tahap 1 hanya memakai layar di depan mata pada jarak baca biasa. **Jangan menempelkan rangkaian elektronik, LED, atau layar ke mata.** Pengujian yang menyentuh mata hanya boleh dilakukan pada tahap lanjutan bersama laboratorium, komite etik, dan dokter mata.
