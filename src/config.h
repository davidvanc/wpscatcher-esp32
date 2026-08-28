#pragma once

// Instellingen, zelfde geest als config.ini in het Pi-project.

// Ssid + wachtwoord als tekst onder de QR tonen. Voorlopig uit: enkel de
// QR, gescand met de telefoon (die toont ssid+ww zelf in de
// wifi-instellingen daar).
constexpr bool SHOW_CREDENTIALS_TEXT = false;

// Wachtwoord er in klare tekst bij tonen (enkel relevant als
// SHOW_CREDENTIALS_TEXT aan staat).
constexpr bool SHOW_PASSWORD = true;

// Scherm blijft aan tot dit verstreken is nadat de QR getoond werd (ms).
// 0 = nooit vanzelf uitschakelen (dan enkel via een knop).
constexpr uint32_t SHUTDOWN_AFTER_MS = 15UL * 1000UL;  // 15 seconden

// Stopt na zoveel mislukte WPS-pogingen zonder ooit verbinding (0 = nooit
// opgeven). Op 0 blijft het toestel bij een router zonder WPS doorzoeken
// tot de batterij leeg is -- bewust zo gelaten tot de eerste test op
// hardware, net als give_up_after in het Pi-project.
constexpr uint8_t GIVE_UP_AFTER_ATTEMPTS = 0;

// Hoogste QR-versie die we toestaan. De code kiest zelf de KLEINSTE versie
// die de payload aankan (kleiner = dikkere modules = beter scanbaar); dit is
// enkel het plafond. Versie 10 = 57x57 modules, 271 bytes op ECC_LOW, ruim
// boven een wifi-payload (ssid 32 + wachtwoord 63 + opmaak).
constexpr uint8_t QR_MAX_VERSION = 10;

// Pixelgrootte van het paneel in mm, om de QR-modules in echte millimeters
// te kunnen loggen -- dat is de maat die in het Pi-project op een telefoon
// geijkt is (0,58 en 0,41 mm lazen, 0,29 mm niet meer).
//
// StickC PLUS SE: 1,14" diagonaal bij 135x240. Diagonaal 28,96 mm, dus het
// zichtbare vlak is 14,2 x 25,2 mm en een pixel meet 14,2/135 = 0,105 mm.
// Staat er een ander bord onder, dan klopt enkel dit getal niet; de rest
// van de code leest de schermmaat zelf uit.
constexpr float SCREEN_PITCH_MM = 0.105f;

// Buffer voor de QR-modules. Moet minstens qrcode_getBufferSize(QR_MAX_VERSION)
// zijn: (4*10+17)^2 bits = 57*57 = 3249 bits = 407 bytes. 512 geeft marge,
// en main.cpp controleert de werkelijke behoefte alsnog bij het tekenen --
// de library zelf bewaakt haar buffergrenzen niet.
constexpr size_t QR_BUFFER_BYTES = 512;
