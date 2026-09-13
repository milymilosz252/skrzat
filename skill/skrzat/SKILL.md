---
name: skrzat
description: Zadaj pytanie na fizycznym pilocie M5StickC Plus2 stojącym na biurku, zamiast pytać w czacie — użytkownik wybiera odpowiedź przyciskami. Użyj, gdy potrzebujesz od użytkownika decyzji (wybór wariantu, zgoda na krok, wybór pliku/gałęzi), a także gdy użytkownik powie "zapytaj na sticku", "wybiorę na pilocie", "daj to na stick". Użyj też do powiadomień o zakończonej pracy ("powiadom na sticku").
---

# Skrzat — pytania i powiadomienia na M5StickC Plus2

Fizyczny pilot na biurku użytkownika. Potrafi wyświetlić pytanie z opcjami; użytkownik
wybiera przyciskiem, a wybór wraca do Ciebie na stdout.

## Pytanie z opcjami

```bash
skrzat ask "Którą bazę wyczyścić?" "dev" "staging" "anuluj"
```

- Blokuje do naciśnięcia przycisku, potem wypisuje **wybraną opcję** na stdout.
- Kody wyjścia: `0` wybrano, `2` odrzucono lub minął czas, `3` stick nieosiągalny.
- Opcje: maksymalnie **8**, krótkie (~16 znaków) — ekran ma 135 px szerokości.
- Flagi: `--title "kontekst"` (mała linia nad pytaniem), `--timeout 300` (sekundy).

Gdy kod wyjścia to `3`, stick jest offline — **wróć do zwykłego pytania w czacie**,
nie ponawiaj w pętli.

## Powiadomienie

```bash
skrzat notify "Testy przeszły, 84/84" --level done
```

`--level info|done|error` steruje kolorem i dźwiękiem. `--seconds N` (domyślnie 6).

## Stan urządzenia

```bash
skrzat status
```

Pokazuje IP, baterię, połączenie z Home Assistant i liczbę obsłużonych pytań.

## Kiedy używać

Używaj, gdy decyzja jest **krótka i wyliczalna** — wybór z 2–8 wariantów. Do pytań
otwartych (wymagających wpisania tekstu) nadal pytaj w czacie: stick nie ma klawiatury.

Nie wysyłaj na stick pytań o rzeczy nieodwracalne bez podania konsekwencji w treści
pytania — użytkownik widzi tylko jedno zdanie i etykiety opcji.
