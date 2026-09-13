#include "i18n.h"
#include "settings.h"

static const char* const PL[S_COUNT] = {
  "Pilot", "Home Assistant", "Claude Code", "Ustawienia", "Szczegoly",
  "Swiatla", "Sceny", "Automatyzacje", "Skrypty", "Media", "Gniazdka",
  "Zgas wszystko", "Pomieszczenia", "Kategorie", "Wszystkie encje", "Bez pomieszczenia",
  "Odswiez z HA", "Jasnosc", "Uspienie", "Dzwiek", "Glosnosc",
  "Zamien gora/dol", "Obrot ekranu", "Oszczedzanie", "Jezyk",
  "Sieci Wi-Fi", "Zmien siec Wi-Fi", "Reset fabryczny", "Restart", "Informacje",
  "wl", "wyl", "n/d", "(pusto)", "tak", "nie", "encji",
  "Przelacz", "Jasniej +20%", "Ciemniej -20%", "Pelna moc", "Przycmij 10%",
  "Wlacz", "Wylacz", "Aktywuj scene", "Uruchom", "Zatrzymaj", "Nacisnij", "Uruchom teraz",
  "Play / Pauza", "Glosniej", "Ciszej", "Nastepny", "Poprzedni", "Wycisz",
  "Otworz", "Zamknij", "Zamknij zamek", "Otworz zamek", "Do bazy", "Pauza",
  "Wyslano", "Wylaczono", "Wlaczono", "Odswiezono", "Zgaszone", "Restart...",
  "Limity", "brak danych", "nieaktualne", "5h", "7d", "za",
  "Status", "Pytan", "Ostatnie pytanie", "pytan w sesji", "dzis",
  "M5:wybierz  M5(dl):ustawienia", "gora/dol  M5:ok  M5(dl):wstecz",
  "M5:szczegoly  M5(dl):wstecz", "gora/dol  M5:zatwierdz", "dowolny przycisk = ok",
  "CLAUDE PYTA", "KONFIGURACJA", "Start...", "Pobieram encje...", "Brak Wi-Fi",
  "Skasowac wszystko?", "Skanuje...", "Aktualizacja firmware",
  "nie odlaczaj zasilania", "Blad aktualizacji",
  "1. Wi-Fi w telefonie:", "2. Otworz w przegladarce:",
  "Podaj siec Wi-Fi, adres HA i token dostepu.",
  "Znajdz Home Assistant", "Znaleziono", "Nie znaleziono",
};

static const char* const EN[S_COUNT] = {
  "Remote", "Home Assistant", "Claude Code", "Settings", "Details",
  "Lights", "Scenes", "Automations", "Scripts", "Media", "Switches",
  "All lights off", "Rooms", "Categories", "All entities", "No room",
  "Refresh from HA", "Brightness", "Sleep", "Sound", "Volume",
  "Swap up/down", "Rotate screen", "Power saving", "Language",
  "Wi-Fi networks", "Change Wi-Fi", "Factory reset", "Reboot", "Info",
  "on", "off", "n/a", "(empty)", "yes", "no", "entities",
  "Toggle", "Brighter +20%", "Dimmer -20%", "Full", "Dim to 10%",
  "Turn on", "Turn off", "Activate scene", "Run", "Stop", "Press", "Run now",
  "Play / Pause", "Louder", "Quieter", "Next", "Previous", "Mute",
  "Open", "Close", "Lock", "Unlock", "Return to base", "Pause",
  "Sent", "Turned off", "Turned on", "Refreshed", "All off", "Rebooting...",
  "Limits", "no data", "outdated", "5h", "7d", "in",
  "Status", "Questions", "Last question", "asks this session", "today",
  "M5:select  M5(hold):settings", "up/down  M5:ok  M5(hold):back",
  "M5:details  M5(hold):back", "up/down  M5:confirm", "any button = ok",
  "CLAUDE ASKS", "SETUP", "Starting...", "Fetching entities...", "No Wi-Fi",
  "Erase everything?", "Scanning...", "Firmware update",
  "do not unplug", "Update failed",
  "1. Wi-Fi on your phone:", "2. Open in a browser:",
  "Enter Wi-Fi, the HA address and an access token.",
  "Find Home Assistant", "Found", "Not found",
};

void langSet(uint8_t lang) { settings.language = lang ? 1 : 0; }
uint8_t langGet() { return settings.language; }

const char* T(Str s) {
  if (s >= S_COUNT) return "?";
  return settings.language ? EN[s] : PL[s];
}
