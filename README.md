# ESP32-S3 Touch LCD 1.85 Audio Combo

Aplicație ESP-IDF pentru kitul `ESP32-S3-Touch-LCD-1.85`, cu Internet Radio, player MP3 de pe card SD și interfață web.

## Interfața pe ecran

- Swipe stânga/dreapta comută între moduri: Radio → MP3 → Asistent AI (punctele de sub etichetă arată modul curent; tap pe etichetă face același lucru); intrarea în modul AI oprește redarea
- Asistent AI cu avatar animat (ascultă/gândește/vorbește): apasă butonul central (microfon) și vorbește — înregistrarea pornește când începi să vorbești și se oprește singură la tăcere; tap pe numele vocii schimbă vocea TTS
- Asistentul poate verifica informații în timp real: vremea și prognoza pe 3 zile pentru orice localitate (Open-Meteo), cursul valutar (BCE/Frankfurter) și cunoaște data și ora locală — întreabă de ex. „Cum e vremea în Cluj?" sau „Cât e euro azi?"
- Ceas sincronizat prin NTP (fus orar România), păstrat în RTC-ul PCF85063 peste reporniri
- Baterie cu iconiță și procent, colorată după nivel (`--` fără baterie)
- Arcul exterior = volum (bula centrală afișează valoarea la reglare)
- Arcul portocaliu de jos = progresul melodiei MP3 curente
- Spinner central la bufferarea stream-ului radio; iconița WiFi din status arată starea conexiunii
- Long-press pe centru (în modurile Radio/MP3) deschide o listă derulantă din care alegi direct postul sau melodia
- Screensaver după 30 s de inactivitate: luminozitatea scade și rămâne doar ceasul + ce se redă; orice atingere revine la ecranul principal

## Hardware

- LCD rotund ST77916 QSPI, 360x360, folosind driverul oficial din demo
- Touch CST816: SDA GPIO1, SCL GPIO3, INT GPIO4
- DAC PCM5101: BCLK GPIO48, LRCK GPIO38, DATA GPIO47
- SDMMC 1-bit: CLK GPIO14, CMD GPIO17, D0 GPIO16

## Card SD

Pune fișierele MP3 în `/` sau `/music`. Opțional:

`/radio.txt`:
```text
Radio Name|http://stream.example/radio.mp3
```

`/wifi.txt`:
```text
ssid=NumeleRetelei
password=Parola
```

`/openai.txt` (cheia pentru asistentul AI — nu o pune în `sdkconfig`/`Kconfig`, ca să nu ajungă în firmware sau în git):
```text
key=sk-...
```

Fără credențiale valide, placa pornește AP-ul `Spotpear-Radio`, parola `12345678`, cu portal la `http://192.168.4.1`.

## Web UI

După conectare, monitorul serial afișează IP-ul. Web UI (temă întunecată, în română, optimizată pentru telefon) oferă:

- bară „acum se redă" cu metadata live (post + titlu stream ICY sau melodie MP3), mod, baterie și progres MP3
- controale prev / play-pauză / next / stop și slider de volum sincronizat cu ecranul
- posturi salvate cu evidențierea postului activ, confirmare la ștergere și notificări de confirmare
- căutare Radio Browser, adăugare URL direct, listă MP3 și setări WiFi
- încărcare fișiere MP3 pe card (cu progres) și ștergere de pe card, direct din browser
- setări: durata până la screensaver (salvată în NVS) și cheia OpenAI (scrisă în `/openai.txt` pe SD)
- la conectare, IP-ul apare câteva secunde și pe ecranul dispozitivului

API: `GET /api/state|tracks|stations|settings`, `POST /api/playpause|stop|prev|next|volume|settings|openai_key|radio/play|radio/test|radio/save|radio/delete|mp3/play|mp3/delete|mp3/upload?name=`.

## Build

```powershell
$env:IDF_PATH='C:\Espressif\frameworks\esp-idf-v5.5.4'
$env:IDF_PYTHON_ENV_PATH='C:\Espressif\python_env\idf5.5_py3.11_env'
. "$env:IDF_PATH\export.ps1"
idf.py -B build_185 build
```

Flash: `idf.py -B build_185 -p COMx flash monitor`.

Notă: asistentul vocal folosește microfonul I2S onboard (BCLK GPIO15, WS GPIO2, DIN GPIO39) și necesită cheia OpenAI în `/openai.txt` pe cardul SD.
