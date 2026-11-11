# CO₂ colour scale: where the thresholds come from

Research notes behind the `co2_scale[]` table in `main/co2_color.c`.
Intended use of the device: **homes and offices**.

---

## 1. The ramp as implemented

```c
static const co2_stop_t co2_scale[] = {
    {    0, 0x76, 0xB0, 0x43 },   // green
    {  800, 0x76, 0xB0, 0x43 },   // end of green: IDA 1 and 2 fall inside
    { 1000, 0xF2, 0xD0, 0x2E },   // full yellow at the most quoted threshold
    { 1400, 0xF2, 0xD0, 0x2E },   // end of the yellow plateau
    { 1700, 0xE0, 0x7B, 0x26 },   // orange
    { 2000, 0xE0, 0x7B, 0x26 },   // end of "elevated" per the UBA
    { 2500, 0x9E, 0x2B, 0x25 },   // "unacceptable": full red
    { 5000, 0x9E, 0x2B, 0x25 },   // stays red from here on
};
```

Piecewise linear interpolation. **Plateaus** (two points with the same colour)
keep the colour steady within a band, and **transitions** (two points with
different colours) make it change continuously. There are no abrupt jumps when
a threshold is crossed, but the colour does change visibly where it matters.

See [`threshold-ramp.png`](threshold-ramp.png) for the full sweep from 400 to
3000 ppm.

---

## 2. Important: absolute versus relative

**RITE and EN 16798-1 define their limits as ppm _above outdoor air_, not as
absolute values.** It is an easy detail to miss, and it shifts the figures by
several hundred ppm.

Atmospheric CO₂ is around **420 ppm** (about 430 in 2026) and rises by about
2.5 ppm a year. All conversions in this document use 420:

```
absolute = regulatory limit + 420
```

A sensor can only measure the absolute value, so the firmware table is
necessarily absolute. As a consequence, **the thresholds age**: in a decade
they would be about 25 ppm off. That is irrelevant in practice, but it explains
why the standards are written in relative terms.

---

## 3. The IDA categories of the RITE (Spain)

The **RITE** (*Reglamento de Instalaciones Térmicas en los Edificios*, the
Spanish regulation on thermal installations in buildings, RD 1027/2007) sorts
indoor spaces into four air quality categories, the **IDA** (*Indoor Air
Quality*), in its technical instruction **IT 1.1.4.2.2**. The category is not
the installer's choice: **it follows from the building's use**.

| Category | Quality | Typical uses |
|---|---|---|
| **IDA 1** | Optimal | Hospitals, clinics, laboratories, nurseries |
| **IDA 2** | Good | Offices, residences, classrooms, museums, reading rooms, swimming pools |
| **IDA 3** | Average | Shops, cinemas, theatres, restaurants, bars, gyms, hotels |
| **IDA 4** | Low | Spaces with lower comfort requirements |

Reaching the required category takes a minimum outdoor air flow, and
**IT 1.1.4.2.3** allows several calculation methods. One of them is the
**indirect method by CO₂ concentration**, which the regulation itself justifies
because CO₂ is a *"good indicator of human bioeffluent emissions"*. Its values
are in **table 1.4.2.3**:

| Category | ppm above outdoor | ≈ absolute (outdoor 420) |
|---|---|---|
| IDA 1 | ≤ 350 | **≤ 770** |
| IDA 2 | ≤ 500 | **≤ 920** |
| IDA 3 | ≤ 800 | **≤ 1220** |
| IDA 4 | ≤ 1200 | **≤ 1620** |

**For this device the reference is IDA 2** (offices and homes): about 920 ppm
absolute.

---

## 4. EN / CEN 16798-1 (2019)

The European standard that replaced EN 15251, and the technical reference for
the Energy Performance of Buildings Directive (EPBD). It defines four indoor
environmental quality categories, also **above outdoor air**:

| Category | ppm above outdoor | ≈ absolute |
|---|---|---|
| Cat I | 550 | ~970 |
| **Cat II** | **800** | **~1220** |
| Cat III and IV | 1350 | ~1770 |

The standard itself points out that these values are **indicators of the
ventilation rate per person**, not health limits, and that **Category II
matches normal expectations** of the occupants.

---

## 5. UBA (Germany), in absolute values

The *Ad-hoc-Arbeitsgruppe Innenraumrichtwerte* of the Umweltbundesamt published
its health assessment of CO₂ in indoor air in 2008. It is the most quoted
source in **absolute** values, and the one that best fits what a sensor can
show:

| Band | Classification | Action |
|---|---|---|
| < 1000 ppm | *Hygienisch unbedenklich*: hygienically harmless | None |
| 1000 – 2000 ppm | *Hygienisch auffällig*: elevated | Ventilate |
| > 2000 ppm | *Hygienisch inakzeptabel*: unacceptable | Ventilate urgently |

Two useful facts from the same document: in the 1000–2000 ppm band there is a
**loss of concentration and performance**, and general well-being gets worse;
and **already at 1000 ppm about 20 % of people can be expected to be
dissatisfied**. The recommendation is to ventilate *before* reaching 1000.

---

## 6. Pettenkofer and the origin of "1000 ppm"

The 1000 ppm figure dates from the 19th century. **Max Josef von Pettenkofer**
proposed 1000 ppm as a marker of inadequate ventilation, and **700 ppm for
bedrooms**.

The nuance matters, because it changes how everything above should be read:
Pettenkofer explicitly held that **CO₂ is not the cause** of the ill effects of
stale air, but a **measurable proxy** for the organic matter given off by skin
and lungs. Later work (Lemberg, Yaglou) established CO₂ as a marker of the
**perception of body odour**.

In other words: **at these concentrations CO₂ is not a toxin, it is a
tell-tale**. It shows how much exhaled air you are breathing, and therefore how
well the air is being renewed. Direct physiological effects of CO₂ appear at
concentrations orders of magnitude higher.

---

## 7. ASHRAE's objection

Worth knowing, because it is the most authoritative voice in the field and it
**disagrees**.

Its *Position Document on Indoor Carbon Dioxide* states that the CO₂ values in
Standard 62.1 *"are only for the purposes of implementing CO2 DCV"*
(demand-controlled ventilation) and *"are not intended to be and should not be
used as indicators of IAQ"*.

And on CO₂ as an indicator of infection risk, popularised during the pandemic:
*"ASHRAE does not recommend a specific CO2 concentration as a metric of
infection risk or ventilation adequacy"*.

Other organisations did issue guidance during the pandemic (CDC, REHVA,
EMG/SPI-B in the United Kingdom). The **SAGE-EMG** one is the most quoted:
**< 1000 ppm** in general and **< 800 ppm** in spaces with high aerosol
generation.

**How this fits here**: ASHRAE's position is reasonable for designing building
installations, since CO₂ says nothing about VOCs, particles or radon. But for a
home monitor, CO₂ is still the best low-cost indicator of *air renewal*, which
is what this device sets out to show. What it must not do is present CO₂ as an
"air quality" index in the broad sense.

---

## 8. Why these thresholds

**Green up to 800.** This is where every reference still considers the air
good: IDA 1 (770) and IDA 2 (920) fall inside or right at the edge, and it
matches the strict SAGE-EMG limit. An earlier version of the ramp ended green
at 550, barely 130 ppm above outdoor air, and turned perfectly ventilated rooms
yellow.

**Full yellow at 1000.** The most quoted threshold in the literature
(Pettenkofer, UBA, SAGE) and the point where the UBA moves from "harmless" to
"elevated". It deserves an unmistakable visual signal. In the earlier ramp it
fell in the middle of a plateau, and crossing it changed nothing.

**Yellow plateau up to 1400.** Covers the IDA 3 band (1220) and approaches
IDA 4 (1620): mediocre air, but not alarming.

**Orange at 1700–2000.** The last stretch of what the UBA calls "elevated".

**Red from 2000, full red at 2500.** 2000 is the UBA's boundary for
*unacceptable*. The earlier ramp did not reach red until 2400, 400 ppm late.

---

## 9. If the intended use changes

The thresholds depend on where the device is used:

- **Bedroom**: Pettenkofer recommended 700 instead of 1000. It would make sense
  to bring yellow forward, for example green up to 600 and full yellow at 800.
- **Classroom or nursery**: IDA 1 applies (770 absolute). Same downward shift.
- **Shop or gym**: IDA 3 (1220) allows a more lenient scale.

Editing `co2_scale[]` is enough. The only technical constraint is that the ppm
values must be in **strictly increasing order**, because `co2_color()` divides
by the distance between two consecutive points.

---

## Sources

- [RITE, Real Decreto 1027/2007, Instrucciones Técnicas](https://normatia.com/es/normativa/rite/2022/parte-ii-instrucciones-tecnicas/): IT 1.1.4.2.2 (IDA categories) and IT 1.1.4.2.3 with table 1.4.2.3 (CO₂ method). In Spanish.
- [ASHRAE, Position Document on Indoor Carbon Dioxide](https://www.ashrae.org/file%20library/about/position%20documents/pd-on-indoor-carbon-dioxide-english.pdf): CEN 16798-1 categories, Pettenkofer's historical origin and ASHRAE's position.
- [CEN 16798-1:2019](https://www.une.org/encuentra-tu-norma/busca-tu-norma/norma?c=N0062875): Energy performance of buildings, Ventilation for buildings, Part 1.
- [Umweltbundesamt, Gesundheitliche Bewertung von Kohlendioxid in der Innenraumluft (2008)](https://www.umweltbundesamt.de/system/files/medien/pdfs/kohlendioxid_2008.pdf). In German.
- [Umweltbundesamt, Hygienische Leitwerte für die Innenraumluft](https://www.umweltbundesamt.de/sites/default/files/medien/4031/bilder/dateien/hygienische_leitwerte_0.pdf). In German.
- [Sensirion, SCD30 Datasheet](https://sensirion.com/media/documents/4EAF6AF8/61652C3C/Sensirion_CO2_Sensors_SCD30_Datasheet.pdf): sensor range and accuracy, ±(30 ppm + 3 %) between 400 and 10,000 ppm.
