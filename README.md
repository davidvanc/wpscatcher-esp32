# wpscatcher-esp32

Zelfde idee als [`../wpscatcher`](../wpscatcher): via WPS verbinden en de
wifi-gegevens als QR tonen. Andere hardware: een **M5StickC PLUS SE**
(ESP32-PICO-D4, 1,14" LCD 135×240, 120mAh accu, AXP192, 48×24×8,4 mm in een
afgewerkte behuizing), ipv de Pi Zero + los e-ink-paneel.

Waarom dit bordje:
- **Geen e-ink meer nodig** — het toestel draait hooguit een paar minuten,
  dus een LCD volstaat. Dat lost meteen het echte ongemak van de Pi-versie
  op: e-ink houdt het beeld vast zonder stroom, dus je moet wachten tot het
  netjes afsluit (`blank_before_shutdown`) voor je de stekker mag trekken.
  Een LCD toont niets meer zodra de stroom weg is — knop C indrukken en
  klaar.
- **Kleiner en alles-in-één**: 54×54×18mm, scherm/batterij/behuizing
  inbegrepen, geen Pi + los paneel meer te stapelen.
- **Milliseconden opstarttijd** — geen Linux, geen SD-kaart.
- Verkrijgbaar bij Reichelt (BE/DE), dus EU-bevoorraad.

## Het krappe punt: hoe groot wordt die QR echt

Dit scherm is klein, en dat is de enige eigenschap die dit toestel slechter
maakt dan de Pi met 2,13" e-ink. Uitgerekend, niet geschat:

1,14" diagonaal bij 135×240 geeft een vlak van 14,2 × 25,2 mm, dus een pixel
meet 0,105 mm. De QR is vierkant en wordt dus begrensd door de korte kant:
maximaal 127 px ≈ **13,4 mm**.

Wat dat oplevert, met de payload `WIFI:T:WPA;S:<ssid>;P:<ww>;;` (18 tekens
opmaak + ssid + wachtwoord):

| ssid + wachtwoord | QR-versie | modules | px/module | mm/module |
|---|---|---|---|---|
| tot 35 tekens | 3 | 29 | 4 | **0,42** |
| 36–60 tekens | 4 | 33 | 3 | **0,32** |
| 61–88 tekens | 5 | 37 | 3 | 0,32 |

In het Pi-project is dit op papier geijkt: **0,58 en 0,41 mm lazen, 0,29 mm
niet meer**, met de faalgrens rond 0,3–0,4 mm. Op grond daarvan verwachtte ik
dat die tweede rij — 0,32 mm — randgeval zou zijn.

**Dat viel mee: getest met een wachtwoord van 45 tekens en het scande gewoon.**
Die zit met 0,32 mm per module onder de papieren faalgrens en werkt toch. De
verklaring is dat de ijking op papier pessimistisch is voor dit scherm:
inktspreiding en rafelige randen tegenover harde pixelgrenzen en
achtergrondverlichting.

De firmware logt bij elke QR de gekozen versie en de mm per module, en
waarschuwt nog steeds onder 0,40 mm — die waarschuwing is dus strenger dan
wat in de praktijk blijkt te werken.

## Antenne

Een vast antennetje op de PCB, geen externe connector, niet vervangbaar.
Doet er niet toe voor WPS — dat zit in de wifi-driver/esp_wps-laag, niet in
de antenne.

## Waarom M5Unified dit bord kent zonder dat het erin staat

De SE staat **niet** als aparte board in M5GFX (dat kent enkel `M5StickC`,
`M5StickCPlus`, `M5StickCPlus2` en `M5StickS3`). Dat is toch geen probleem,
en dat is nagekeken in de broncode in plaats van gehoopt:

M5GFX herkent een bord aan de ESP32-package en daarna aan de panel-id die
het over SPI uitleest. De Plus2-tak vereist `pkg_ver == 6` (PICO-V3-02) — de
SE heeft een **PICO-D4** en valt daar dus buiten. In de tak daarvoor wordt de
panel-id gelezen: `(id & 0xFB) == 0x81` betekent ST7789, en dan komt het uit
op `board_M5StickCPlus`. De SE heeft precies die ST7789v2 op 135×240.

Dat is ook de juiste uitkomst voor de rest: de SE heeft, net als de PLUS,
een **AXP192** als PMU (de Plus2 is nu juist degene die de AXP192 liet
vallen). Batterijpercentage en het echt uitschakelen lopen dus via dezelfde
chip als bij de PLUS.

Knoppen op dit bord: **BtnA** (GPIO37), **BtnB** (GPIO39) en de aan/uitknop,
die M5Unified als `BtnPWR` aanbiedt via de AXP192 — `cfg.pmic_button` staat
standaard aan. Er is geen BtnC; de code vraagt die wel op, maar dat is
onschadelijk en houdt dezelfde firmware bruikbaar op een Core Basic.

## WPS op ESP32 — bewijs, niet aangenomen

Dit bord (ESP32-D0WDQ6-V3) heeft **native WPS in de officiële SDK**, net
zoals de Pi's `wpa_supplicant`. Twee officiële bronnen van Espressif zelf,
niet een blogpost:

- ESP-IDF: [`examples/wifi/wps/main/wps.c`](https://github.com/espressif/esp-idf/blob/master/examples/wifi/wps/main/wps.c)
- Arduino-ESP32: [`libraries/WiFi/examples/WPS/WPS.ino`](https://github.com/espressif/arduino-esp32/blob/master/libraries/WiFi/examples/WPS/WPS.ino)

`src/main.cpp` in dit project is de WPS-laag (`wpsConfigInit`, `wpsStart`,
`onWifiEvent`) 1-op-1 overgenomen van dat Arduino-voorbeeld, inclusief de
event-namen (`ARDUINO_EVENT_WPS_ER_SUCCESS` e.a. — dat zijn niet dezelfde
namen als in oudere versies van de core, dus dit is met de actuele
masterbron op 2026-08-26 gecontroleerd) en de IDF-versie-afhankelijke
signature van `esp_wifi_wps_start()`.

Net als bij de Pi geeft de router na een succesvolle PBC-uitwisseling het
**plaintext wachtwoord** terug (niet een hash) — dat komt hier binnen via
`esp_wifi_get_config()` op `conf.sta.password`, het ESP-IDF-equivalent van
de `psk="..."` regel die wpa_supplicant op de Pi wegschreef.

## Hoe het werkt

Scherm 1 "WPS zoeken" → `esp_wifi_wps_start()` → event `WPS_ER_SUCCESS` of
`WPS_ER_FAILED`/`WPS_ER_TIMEOUT` → gelukt: ssid+wachtwoord uitlezen, QR
bouwen (`WIFI:T:WPA;S:...;P:...;;`, met escaping van `\ ; , : "`), scherm 2
tonen → mislukt: opnieuw proberen (teller `attempt`, instelbaar
maximum via `GIVE_UP_AFTER_ATTEMPTS` in `config.h`).

Na `SHUTDOWN_AFTER_MS` (standaard 5 minuten) na het tonen van de QR gaat
het toestel vanzelf uit via `M5.Power.powerOff()` — dat schakelt de
voeding via de PMIC echt uit, anders dan de Pi die in halt-toestand nog
15-25 mA blijft trekken.

Knop A: opnieuw proberen na een mislukte poging. Knop C: direct
uitschakelen.

## Bouwen

```bash
pio run -e core-basic -t upload
```

Voor een StickC in plaats van een Core Basic: `-e stickc`. Meekijken met
`pio device monitor` — de firmware logt daar de schermmaat, de gekozen
QR-versie en het aantal pixels per module.

Vereist [PlatformIO](https://platformio.org/). `platformio.ini` haalt
`M5Unified` (scherm/knoppen/voeding) en `ricmoo/QRCode` (QR-rendering) op.

## Werkend, 28-08-2026

Volledige keten op hardware, op twee toestellen: opstarten → "WPS zoeken" →
knop op de modem → QR → zichzelf uitgeschakeld. Ook getest met een
wachtwoord van 45 tekens.

Draaiende instelling: `SHUTDOWN_AFTER_MS` op **30 s** — korter kan niet,
want een telefooncamera moet eerst scherpstellen op het schermpje. Geen
credentials als tekst op het scherm.

Gemeten in plaats van geschat:

| | |
|---|---|
| Boot, reset tot firmwarebanner | **646 ms** |
| Schermmaat zoals de firmware ze leest | 240×135 |
| Bord volgens de fabrieksfirmware | `board: 4` = `board_M5StickCPlus` |
| Flash | 72,3% van 1,3 MB · RAM 14,6% |

Die 646 ms tegenover 2 min 03 voor de Pi zoals geleverd, en ~35 s na alle
ingrepen daar.

**De AXP192-uitschakeling is bevestigd**, en wel per ongeluk: een tweede
flashpoging faalde met `No serial data received` terwijl COM5 er nog gewoon
was. De FTDI-chip hangt aan USB-stroom en blijft dus zichtbaar, de ESP32 was
echt uit. Staat het toestel zo, druk dan de aan/uitknop in voor je flasht.

## Logboek

Elke geslaagde WPS wordt weggeschreven naar flash, met datum en uur uit de
BM8563-RTC. Uitlezen gaat via de seriële poort (115200), met deze commando's:

| | |
|---|---|
| `dump` | de hele lijst tonen, als CSV |
| `wis` | het logboek wissen |
| `tijd` | tonen wat de RTC nu denkt |
| `tijd 2026-08-28 19:35:56` | de RTC gelijkzetten |

Het formaat is CSV met aanhalingstekens, dus een ssid of wachtwoord met
komma's of aanhalingstekens erin breekt het bestand niet:

```
datum,tijd,ssid,wachtwoord
"2026-08-28","19:35:56","MijnNetwerk","w4chtw00rd"
```

**De tijd moet je één keer zetten.** Het toestel gaat nooit online, dus er is
geen NTP. De RTC loopt daarna door op de batterij; raakt die helemaal leeg,
dan komt er `onbekend` in de kolommen datum en tijd te staan in plaats van
een verzonnen datum.

### Capaciteit en gedrag bij een vol bestandssysteem

De partitie is 1.441.792 bytes, waarvan 1.433.600 bruikbaar. Een regel kost
30 bytes opmaak plus ssid plus wachtwoord:

| | regel | aantal regels |
|---|---|---|
| ssid 15, wachtwoord 45 | ~90 bytes | ~15.800 |
| ssid 32, wachtwoord 63 (maximum) | 125 bytes | ~11.400 |

Loopt het toch vol, dan **blijft het toestel gewoon werken**: onder
`LOG_MIN_FREE_BYTES` (8 KB) wordt de regel overgeslagen met een melding op de
seriële poort, en de QR verschijnt zoals altijd. Het logboek is bijzaak.

Het opstarten wordt er niet trager van: 635 ms zonder logboek, 665 ms met.
Bij de start wordt bewust alleen de bestandsgrootte uit de metadata gelezen
en worden de regels **niet** geteld — dat laatste leest het hele bestand en
zou de start dus trager maken naarmate het logboek groeit. Tellen gebeurt
enkel bij `dump`.

### Waarschuwing

Dit bewaart wachtwoorden van klanten in klare tekst in flash, en dat
overleeft het uitschakelen. Raak je het toestel kwijt, dan heeft de vinder
elk netwerk waar je geweest bent. Dat is een bewuste afweging en het staat
haaks op wat de Pi-versie doet, die het scherm juist wist om die reden. Zet
`LOG_ENABLED` op `false` in `config.h` als je dat niet wil.

## Bij het flashen: let op welke FTDI

De PLUS SE gebruikt een **FTDI**-chip (VID_0403, PID_6001) — niet de CH9102
of CP2104 die je bij zulke bordjes zou verwachten. Ligt er ook een gewone
usb-serieel-adapter aangesloten, dan hebben die exact hetzelfde VID en PID en
zijn ze enkel aan het serienummer te onderscheiden. Zet `--upload-port` dus
altijd expliciet en vertrouw niet op automatische detectie.

## Nog open

- **Meerdere WPS-credentials**: geeft de router er meer dan één terug, dan
  roept WPS `esp_wifi_set_config()` niet zelf aan en leest
  `readWpsCredentials()` mogelijk een lege config. Zeldzaam bij
  consumentenrouters, niet afgedekt.
- **Hex-PSK-router**: als de router geen plaintext passphrase teruggeeft
  (zeldzaam, zie de Pi-README), blijft `conf.sta.password` leeg. De firmware
  logt dat en toont een QR die niet werkt; er is geen terugval ingebouwd.
- Geen tegenhanger van `test_render.py`/`test_scan.py` — de payload-escaping
  wordt hier door niets teruggelezen.
- Tijdens het zoeken logt de firmware niets naar serieel; een time-out
  verhoogt enkel de teller op het scherm. Handig om toe te voegen als je ooit
  op afstand wil zien of de lus nog draait.

## Batterij

120 mAh. Tijdens actief wifi + scherm trekt zo'n ESP32 grofweg 200–300 mA,
dus reken op **een half uur totale looptijd**, oftewel een stuk of 5 à 7
beurten van vijf minuten — en met `shutdown_after` op 60 s eerder een stuk
meer. Dat is een schatting uit algemene ESP32-cijfers, geen fabrieksspec:
M5Stack publiceert enkel de 120 mAh.

Opladen gaat via USB-C. De 1,4 A die bij de PLUS vermeld staat is de
ingangsstroom van de PMU, niet wat er naar de cel gaat — 120 mAh op 1,4 A
zou meer dan 10C zijn en dat doet geen enkele beschermde LiPo. Realistisch
is 1 à 1,5 uur. Het ledje gaat van rood naar groen.

Het batterijpercentage staat rechtsboven op elk scherm.
