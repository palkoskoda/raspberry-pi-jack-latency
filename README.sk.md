# Rýchly nástup MIDI cez analógový jack Raspberry Pi 3

[Podrobný technický článok](README.md) · [Namerané dáta](measurements/README.md) · [Príklady kódu](examples/) · [Ovládač PWM/DMA](driver/)

Na fyzickom Raspberry Pi 3 Model B sme pri 48 izolovaných tónoch namerali **p95 3,594 ms a maximum 7,031 ms** od prijatia celej MIDI správy v aplikácii po odhad času odovzdania prvého nenulového rámca v DMA. Pod 4 ms bolo 46 tónov. Číselná skúška použila virtuálny vstup. Hráč na skutočnom Rolande potvrdil citeľne rýchlejšiu odozvu pri predchádzajúcej verzii. Fyzický čas od stlačenia klávesu po napätie na jacku sme zatiaľ nezmerali.

## Čo sme zmenili

1. **Pravá ruka má krátku cestu.** Po zostavení MIDI správy rozhodne hudobné jadro o registri, vrstve, transpozícii, dynamike a pedáli. Rust zaradí udalosť na najbližšiu zapisovateľnú snímku. Syntetizátor vyráta suchý nástup v krátkom bloku. Reverb a chorus môžu prísť o pár milisekúnd neskôr. Prednačítané zvuky a krátky kód bez čakania udržia túto cestu predvídateľnú.
2. **Sprievod sa počíta vopred.** V režime AUTO sa udalosti plánujú približne 60 ms vopred a samostatný renderer drží zásobu najviac 40 ms hotového PCM. Blok má index snímky a identitu prehrávania. Pri Stop sa čakajúci sprievod zneplatní. Znejúce pravé hlasy a ich uvoľnenia patria stále živému rendereru.
3. **Hĺbka fronty sa mení bez zmeny fyzického kruhu.** ALSA vyjednala pri 88,2 kHz periódu 32 snímok a kapacitu 1 024 snímok. Za normálu sme držali 128 snímok, čiže približne 1,45 ms. Pri podtečení sa obnoví ten istý PCM tok a cieľ sa zväčší. V skúške stúpol zo 128 na 192 snímok a znejúci tón pokračoval bez ďalšieho Note On. Po približne 30 sekundách plynulých zápisov klesla hĺbka o jednu periódu. [Rozhranie ALSA PCM](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)
4. **Priamy PWM ovládač obchádza frontu pôvodného firmvéru.** DMA číta cyklický kruh podľa tempa PWM FIFO. Ovládač vie vrátiť pozíciu DMA, takže ALSA má jemnejší údaj o priebehu. Pri 88,2 kHz pripadajú na snímku dva intervaly PWM nosnej. Akumulátor zlomkovej časti rozloží medzihodnoty úrovne do následných intervalov. Posluch zaznamenal menej šumu pod tónom. [Schéma Pi 3B](https://www.raspberrypi.org/documentation/hardware/raspberrypi/schematics/rpi_SCH_3b_1p2_reduced.pdf) · [Linux DMAengine](https://docs.kernel.org/driver-api/dmaengine/client.html)

## Čo naozaj limitovalo výsledok

- Na pôvodnej ceste sa pri agresívnom skracovaní buffra vyskytli chyby hlásené firmvérom, hoci testovacie vlákno po zobudení dostalo CPU do 9 µs. To nasmerovalo prácu na ovládač. [Zdroj pôvodného ovládača](https://github.com/raspberrypi/linux/blob/cff533aec2fa601846766b32ff57204e0a61bed7/drivers/staging/vc04_services/bcm2835-audio/bcm2835-pcm.c#L52-L80)
- Po zrýchlení jacku už najpomalšie noty často čakali v JVM alebo pri odovzdaní udalosti do Rustu. Zníženie fronty zo 128 na 96 snímok môže ušetriť asi 0,36 ms. Jedno také meranie malo p95 5,97 ms, lebo sa zhoršil práve tento úsek. Väčší potenciál má kratšie odovzdanie udalosti, meranie zobudení a príprava stavu pri zmene registra.
- Samotná vyššia priorita v JVM pomohla MAN a v tom pokuse vznikla v sprievode medzera 56 ms. Premiestnenie renderera sprievodu na iné jadro zachovalo rýchly MAN a v dvoch krátkych priechodoch odstránilo merané medzery po začiatku sprievodu. Použité priority sú príklad pre jedno Pi. Najprv zmeraj rozloženie vlákien a prerušení svojho systému. [Plánovanie a izolácia jadier](https://docs.kernel.org/admin-guide/cpu-isolation.html)
- Pri malej fronte sa prejavila aj pamäť. Stopa jadra zachytila načítanie stránky a čakanie na presun uzamknutej stránky. Pomohlo zahriať vybrané mapovania kódu a držať vzorky pripravené. Pred systémovými zmenami je vhodné overiť rezervu RAM a odstrániť súborové operácie, logovanie a dlhé zámky zo zvukovej slučky. [Nastavenia správy pamäte Linuxu](https://docs.kernel.org/admin-guide/sysctl/vm.html#compact-unevictable-allowed)

## Použitie pre vlastný projekt

[Príklad v C](examples/adaptive_pcm.c) ukazuje krátku frontu vo väčšom ALSA kruhu, obnovu pri podtečení a pokles rezervy po stabilnej hre. [Analyzátor WAV](examples/measure_loopback.py) vezme referenčný impulz na prvom kanáli a výstup jacku na druhom kanáli toho istého záznamu. Vráti percentily aj neúplné alebo nejednoznačné pokusy. Obe ukážky sa dajú skúšať bez úprav ovládača. [Príkazy a zapojenie](examples/README.md)

Jedna trojbajtová správa cez 5-pinový DIN trvá na vodiči približne 0,96 ms. Oproti USB adaptéru vie správne oddelený UART zmeniť jitter, samotný prenos DIN zostáva. Na inej zvukovke porovnaj skutočný čas pri rovnakej elektrickej referencii. I²S DAC môže zlepšiť kvalitu výstupu a jeho celková latencia závisí aj od fronty ovládača a digitálneho filtra. [Špecifikácia MIDI](https://midi.org/wp-content/uploads/wpforo/default_attachments/1709416667-ca33-MIDI-10-Electrical-Specification-Update.pdf) · [PCM5102A](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf)
