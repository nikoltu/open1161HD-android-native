package org.openbw.android;

import java.util.HashMap;
import java.util.Map;

/**
 * Launcher string translation (v0.80).
 *
 * The launcher builds every view programmatically and has no res/layout or
 * res/values at all, so Android string resources would add an XML layer without
 * removing any of the edits. A plain table also serves the native side, which
 * cannot reach resources, so both halves use the same mechanism.
 *
 * The KEY is the original Lithuanian text, because that is what the source
 * already contained; translating rather than re-keying meant the pass could not
 * accidentally alter a string's meaning. A key with no entry falls through
 * unchanged, so a missed string shows up in Lithuanian - visible, and harmless.
 *
 * Only strings that reach setText/setHint/dialog builders were wrapped.
 * Preference keys are never passed to those, so switching language cannot
 * rename a key and lose profiles or campaign progress.
 */
final class S {
    static final String EN = "en";
    static final String LT = "lt";
    private static String lang = EN;

    private S() {}

    static void setLanguage(String l) { lang = LT.equals(l) ? LT : EN; }
    static String language() { return lang; }

    static String t(String lithuanian) {
        if (LT.equals(lang)) return lithuanian;
        String en = EN_MAP.get(lithuanian);
        return en != null ? en : lithuanian;
    }

    private static final Map<String, String> EN_MAP = new HashMap<>();
    private static void p(String lt, String en) { EN_MAP.put(lt, en); }

    static {
        // --- tabs and chrome ---
        p("SINGLE PLAYER", "SINGLE PLAYER");
        p("MULTIPLAYER", "MULTIPLAYER");
        p("KAMPANIJA", "CAMPAIGN");
        p("MODAI", "MODS");
        p("REPLAY", "REPLAY");
        p("NUSTATYMAI", "SETTINGS");
        p("PRADĖTI ŽAIDIMĄ", "START GAME");
        p("Profilis (UserID):", "Profile (UserID):");
        p("Keisti", "Change");
        p("Naujas", "New");
        p("Naujas profilis", "New profile");
        // --- launcher redesign F7: saved games + replays ---
        p("Išsaugoti replay'ai", "Saved replays");
        p("Paieška: vardas, žemėlapis, žaidėjas…", "Search: name, map, player…");
        p("+ Importuoti (.rep)", "+ Import (.rep)");
        p("Pasirink išsaugotą žaidimą kairėje.", "Pick a saved game on the left.");
        p("Išsaugotas žaidimas", "Saved game");
        p("Misija", "Mission");
        p("Žemėlapis", "Map");
        p("Žaidimo laikas", "Game time");
        p("Išsaugota", "Saved");
        p("Failas", "File");
        p("Tęsiant kampanijos seivą misija pažymima kaip dabartinė — įveikus ji gaus varnelę.", "Continuing a campaign save marks its mission as the current one — beating it earns the tick.");
        p("Pašalinti išsaugotą žaidimą?", "Delete the saved game?");
        p("Antraštė nenuskaityta", "Header unreadable");
        p("Paskutinis žaidimas", "Last game");
        p("Išsaugoti", "Save");
        p("klaida", "error");
        p("prieš AI — atkurti negalima", "vs AI — cannot be replayed");
        p("neišsaugotas", "unsaved");
        p("Trukmė", "Length");
        p("Data", "Date");
        p("Dydis", "Size");
        p("Žiūrėti", "Watch");
        // --- redundant-text pass ---
        p("Peržiūra neprieinama", "Preview unavailable");
        p("Iki 72 vienetų pažymėjime", "Up to 72 units in a selection");
        p("Kelių pastatų žymėjimas — pastatai vienoje selekcijoje", "Multi-building select — buildings share one selection");
        p("Hostai", "Hosts");
        p("Hostų nerasta. Kitame įrenginyje paspausk „Sukurti lobby“.", "No hosts found. On the other device press “Create lobby”.");
        p("Užrakinta — pirma įveik ankstesnę misiją.", "Locked — beat the previous mission first.");
        p("prieš AI", "vs AI");
        p("Įdiegta: ", "Installed: ");
        p("failas sugadintas, bandyk dar kartą", "the file is damaged, try again");
        p("Talpykla nepasiekiama: ", "Repository unreachable: ");
        p("Raktas lieka tik šiame įrenginyje.", "The key stays on this device only.");
        p("SC:R rasta — HD įjungta.", "SC:R found — HD on.");
        p("SC:R aplankas nustatytas", "SC:R folder set");
        p("Profiliai ir progresas lieka.", "Profiles and progress are kept.");
        p("Suteik „Visų failų prieigą“ ir vėl paspausk „Pasirinkti SC:R aplanką“.", "Grant “All files access”, then press “Select SC:R folder” again.");
        p("Tai ne SC:R įdiegimo aplankas. Pasirink aplanką, kuriame yra „data“ ir „.build.info“.", "This is not an SC:R install folder. Pick the folder that contains “data” and “.build.info”.");
        p("Originali meniu muzika.", "The original menu music.");
        p("Mygtukų ir skydelių garsai.", "Button and panel sounds.");
        p("HD2 rekomenduojama. FULL HD reikia SC:R ir galingos planšetės.", "HD2 is recommended. FULL HD needs SC:R and a strong tablet.");
        p("Pastatai ir vienetai meta pakreiptą šešėlį.", "Buildings and units cast a slanted shadow.");
        p("0 = beveik plokščias.", "0 = almost flat.");
        p("Burtai, ugnis ir sprogimai apšviečia aplinką. Tik HD.", "Spells, fire and explosions light the surroundings. HD only.");
        p("0 = išjungta.", "0 = off.");
        p("HD2 grafika atrodo aštriau.", "HD2 graphics look sharper.");
        p("Per stipriai — triukšmas ir aureolės aplink kraštus.", "Too strong — noise and halos around edges.");
        p("Kontrastas, sodrumas, šiluma. 0 = išjungta.", "Contrast, saturation, warmth. 0 = off.");
        p("LAN sklandumas", "LAN smoothing");
        p("Sklandesnis vienetų judėjimas LAN žaidime.", "Smoother unit movement in a LAN game.");
        p("Veiksmų eilės mygtukas žaidimo ekrane.", "The order-queue button on the game screen.");
        p("Bakstelėjimas žemėje ar į priešą duoda komandą pažymėtiems.", "A tap on the ground or an enemy orders the selected units.");
        p("Išjunk, jei naudoji spalvotų ikonų modą.", "Turn off if you use a coloured-icon mod.");
        p("Veiksmai per minutę, po laikrodžiu.", "Actions per minute, under the clock.");
        p("Lieka tik šiame įrenginyje.", "Stays on this device only.");
        p("Visas žemėlapis matomas.", "The whole map is visible.");
        p("Naujas portretas kiekvieną kartą atidarius programą.", "A new portrait every time the app opens.");
        p("Patarėjas", "Advisor");
        p("SC:R įdiegimo aplankas", "SC:R install folder");
        p("Atrakina HD grafiką. Galima pridėti vėliau (Nustatymai ▸ Failai).", "Unlocks HD graphics. Can be added later (Settings ▸ Files).");
        p("Nepavyko sukurti lobby. Ar kitas žaidimas jau hostina?", "Could not create the lobby. Is another game already hosting?");
        p("Ryšys su hostu nutrūko.", "The connection to the host was lost.");
        // --- launcher redesign F9: menu audio ---
        p("MPQ failai rasti.", "MPQ files found.");
        p("Meniu muzika", "Menu music");
        p("Originali pagrindinio meniu muzika launcheryje (garsumas: Garsumas × Muzika).", "The original main-menu music in the launcher (volume: Volume × Music).");
        p("Reikia StarCraft.mpq arba BroodWar.mpq (CD archyvų) — be jų meniu tylus.", "Needs StarCraft.mpq or BroodWar.mpq (the CD archives) — without them the menu is silent.");
        p("Meniu garsai", "Menu sounds");
        p("Mygtukų ir skydelių garsai launcheryje (garsumas: Garsumas × Sąsajos garsai).", "Button and panel sounds in the launcher (volume: Volume × Interface sounds).");
        p("meniu garsas nutildytas · įjungti", "menu sound muted · unmute");
        p("meniu garsas įjungtas · nutildyti", "menu sound on · mute");
        // --- launcher redesign F8: settings ---
        p("Failai", "Files");
        p("Garsumas", "Volume");
        p("Muzika", "Music");
        p("Raiška", "Resolution");
        p("Grafika (HD)", "Graphics (HD)");
        p("SC:R aplankas nepasirinktas — HD valdikliai veiks tik pasirinkus jį (Failai).", "No SC:R folder selected — the HD controls only take effect once it is chosen (Files).");
        p("Veiksmų eilės mygtukas žaidimo HUD'e (kas eiles dėlioja tik S-Pen šoniniu mygtuku — gali slėpti).", "The order-queue button in the game HUD (hide it if you only queue with the S-Pen side button).");
        p("Žaidimo greitis", "Game speed");
        p("LAN žaidime galioja hosto greitis.", "A LAN game runs at the host's speed.");
        p("Rodyti", "Show");
        p("Licencijos", "Licences");
        p("Pilni trečiųjų šalių licencijų tekstai.", "Full third-party licence texts.");
        p("Profiliai, statistika ir kampanijos progresas NEBUS liesti.", "Profiles, statistics and campaign progress will NOT be touched.");
        // --- launcher redesign F7: mods ---
        p("Atnaujinti sąrašą", "Refresh list");
        p("+ Modas (.zip)", "+ Mod (.zip)");
        p("+ Botas (.so)", "+ Bot (.so)");
        p("Talpykla…", "Repository…");
        p("vykdomas kodas", "executable code");
        p("yra atnaujinimas", "update available");
        p("aktyvus", "active");
        p("Modas", "Mod");
        p("Teikia", "Provides");
        p("Priklausomybės", "Dependencies");
        p("Būsenos ženklai: ✓ aktyvus · ○ įdiegtas · ⟳ yra atnaujinimas · ↓ galima parsisiųsti", "Status marks: ✓ active · ○ installed · ⟳ update available · ↓ can be downloaded");
        // --- launcher redesign F7: campaign ---
        p("Peržaisti", "Replay");
        p("įveikta", "completed");
        p("dabartinė misija", "current mission");
        p("prieinama", "available");
        p("užrakinta", "locked");
        p("Pradėti misiją", "Start mission");
        p("Pasirink epizodą.", "Pick an episode.");
        // --- launcher redesign F7: LAN screens ---
        p("Prisijungti pagal IP…", "Join by IP…");
        p("Jungtis", "Connect");
        p("Tavo rasė", "Your race");
        p("Žaidėjų skaičius", "Number of players");
        p("SCBW2 taisyklės (nustato hostas, galioja visiems)", "SCBW2 rules (set by the host, apply to everyone)");
        p("Greitis imamas iš Nustatymų: ", "Speed comes from Settings: ");
        p("Prisijungę žaidėjai jį perima.", "Joining players adopt it.");
        p("Aktyvūs hostai (WiFi)", "Active hosts (WiFi)");
        p("Hostas neatsako: ", "No answer from host: ");
        p("neturi šio žemėlapio", "you lack this map");
        p("kita versija ", "other version ");
        p("modai skiriasi", "mods differ");
        p("Tinklas", "Network");
        p("laukia", "waiting");
        p("Laukiama žaidėjų…", "Waiting for players…");
        p("Visi pasiruošę.", "Everyone is ready.");
        p("Laukiama pasirengimo: ", "Waiting for ready: ");
        p("Pasirink hostą sąraše.", "Pick a host in the list.");
        p("Pasirinktas hostas: ", "Selected host: ");
        p("Pradėti", "Start");
        // --- launcher redesign F7: single player screen ---
        p("Importuoti žemėlapį", "Import a map");
        p("Žemėlapiai", "Maps");
        p("Pasirink žemėlapį kairėje.", "Pick a map on the left.");
        p("Žaidėjai", "Players");
        p("Žaidėjų", "Players");
        p("Dvi komandos", "Two teams");
        p("AI lygis", "AI level");
        p("SCBW2 taisyklės", "SCBW2 rules");
        p("Kelių pastatų žymėjimas", "Multi-building select");
        p("Palaikyk taisyklę — paaiškinimas.", "Long-press a rule for its description.");
        p("Originalus", "Original");
        p("Importuoti botą…", "Import a bot…");
        p("Daugiau", "More");
        p("tu", "you");
        p("iki ", "up to ");
        p("žaidėjų", "players");
        p("žaidėjams tinka", "players · fitting");
        p("Komanda A", "Team A");
        p("Komanda B", "Team B");
        p("Peržiūra neprieinama (native biblioteka)", "Preview unavailable (native library)");
        // --- launcher redesign F6: shell (rail) + first-run window ---
        p("Vienas žaidėjas", "Single player");
        p("Tinklas (LAN)", "Network (LAN)");
        p("Modai", "Mods");
        p("Replay'ai", "Replays");
        p("Nustatymai", "Settings");
        p("Melee · laisva kova", "Melee · free for all");
        p("Tame pačiame WiFi tinkle", "On the same WiFi network");
        p("Šeši epizodai", "Six episodes");
        p("Modai ir botai", "Mods and bots");
        p("Įrašytos partijos", "Recorded games");
        p("Tęsk nuo ten, kur baigei", "Continue where you left off");
        p("Pergalės", "Wins");
        p("SC:R nepasirinkta · SD", "SC:R not selected · SD");
        p("Pradėti žaidimą", "Start game");
        p("žemėlapis nepasirinktas", "no map selected");
        p("Botas", "Bot");
        p("Originalus AI", "Original AI");
        p("žaidėjai", "players");
        p("Pirmas paleidimas", "First run");
        p("Pasirink savo žaidimo failus", "Choose your game files");
        p("Programa neturi ir nesiunčia jokių žaidimo failų — juos nurodai pats iš savo legalios kopijos.", "The app neither ships nor downloads any game files — you point it at your own legal copy.");
        p("būtina", "required");
        p("nebūtina", "optional");
        p("patikrinta", "verified");
        p("nepasirinkta", "not selected");
        p("pagrindiniai duomenys, portretai, meniu garsai", "core data, portraits, menu sounds");
        p("Brood War unitai ir portretai", "Brood War units and portraits");
        p("1.16.1 pataisos", "1.16.1 patch data");
        p("nebūtina · kampanijos, žemėlapiai, muzika", "optional · campaigns, maps, music");
        p("nebūtina · BW kampanijos, muzika", "optional · BW campaigns, music");
        p("trūksta", "missing");
        p("nėra", "none");
        p("Atrakina: žaidimą, portretus, meniu garsus. Su abiem nebūtinais archyvais — kampanijas, 38 žemėlapius ir muziką.", "Unlocks: the game, portraits, menu sounds. With both optional archives — campaigns, 38 maps and music.");
        p("Aplankas", "Folder");
        p("SC:R įdiegimo šaknis (skaitoma vietoje, nekopijuojama)", "SC:R install root (read in place, not copied)");
        p("nepasirinktas", "not selected");
        p("Turinys", "Contents");
        p("nerasta", "not found");
        p("Prieiga", "Access");
        p("„Visų failų prieiga“", "“All files access”");
        p("suteikta", "granted");
        p("dar neprašyta", "not requested yet");
        p("Atrakina: HD grafiką (HD2 / FULL HD), HD portretus, originalų šriftą. Be jos viskas veikia SD pakopa.", "Unlocks: HD graphics (HD2 / FULL HD), HD portraits, the original font. Without it everything runs on the SD tier.");
        p("Galima pridėti vėliau: Nustatymai ▸ Failai.", "Can be added later: Settings ▸ Files.");
        p("patikrinta · HD įjungta", "verified · HD on");
        p("Išeiti", "Exit");
        // --- launcher redesign F1-F2: profile card + portrait gallery ---
        p("Profilis", "Profile");
        p("  ·  paliesk — profilis ir portretai", "  ·  tap — profile and portraits");
        p("paliesk — profilis", "tap — profile");
        p("Palietus — „kalbantis“ klipas.", "Tap to play the talking clip.");
        p("Portretai atsiras importavus žaidimo failus.", "Portraits appear once the game files are imported.");
        p("Keisti portretą", "Change portrait");
        p("Portretas: ", "Portrait: ");
        p("režimas: ", "mode: ");
        p("atsitiktinis kaskart", "random each time");
        p("fiksuotas", "fixed");
        p("Pergalės pagal rasę", "Wins by race");
        p("žaid.", "games");
        p("Kampanijos", "Campaigns");
        p("Pervardyti", "Rename");
        p("Pervardyti profilį", "Rename profile");
        p("Kitas profilis", "Switch profile");
        p("Toks profilis jau yra.", "A profile with this name already exists.");
        p("Portretų galerija", "Portrait gallery");
        p("Režimas", "Mode");
        p("Fiksuotas", "Fixed");
        p("Atsitiktinis kaskart", "Random each time");
        p("Atsitiktinis: karių, herojų ir patarėjų portretas metamas vieną kartą paleidus launcherį.", "Random: a unit, hero or advisor portrait is rolled once per launcher start.");
        p("Pasirinkti", "Select");
        p("portretai", "portraits");
        p("Ramybė", "Idle");
        p("Kalbantis", "Talking");
        p("Kariai ir technika", "Troops and vehicles");
        p("Pastatai (bendras patarėjas)", "Buildings (shared advisor)");
        p("Herojai", "Heroes");
        p("Padarai", "Creatures");
        p("Kariai ir laivai", "Warriors and ships");
        p("Zerg patarėjas", "Zerg advisor");
        p("Pasirinkite profilį", "Choose a profile");
        p("Vardas (rodomas žaidime)", "Name (shown in game)");
        p("Statistika (", "Statistics (");
        p("Žaidimų ", "Games ");
        p("Gerai", "OK");
        p("Atšaukti", "Cancel");
        p("Uždaryti", "Close");
        p("Grįžti", "Back");
        p("Tęsti", "Continue");
        p("Į meniu", "To menu");
        p("Palikti", "Leave");

        // --- single player ---
        p("Importuoti Žemėlapius", "Import maps");
        p("Režimas (melee):", "Mode (melee):");
        p("Režimas:", "Mode:");
        p("Priešininkai:", "Opponents:");
        p("Žaidėjų skaičius:", "Number of players:");
        p("Žaidėjai ir rasės:", "Players and races:");
        p("Žaidėjai:", "Players:");
        p("Žaidėjų ", "Players ");
        p("Dvi komandos (kitaip FFA)", "Two teams (otherwise FFA)");
        p("Žemėlapiai:", "Maps:");
        p("Žemėlapis:", "Map:");
        p("Žemėlapis: ", "Map: ");
        p("Žemėlapis:  ", "Map:  ");
        p("vs AI (kompiuteris)", "vs AI (computer)");
        p("Botas: ", "Bot: ");
        p("AI: ", "AI: ");
        p("Labai lengvas", "Very easy");
        p("Lengvas", "Easy");
        p("Vidutinis", "Medium");
        p("Sunkus", "Hard");
        p("Pasirinkite botą", "Choose a bot");
        p("Importuoti botą", "Import a bot");
        p("Be AI", "No AI");
        p("(tu)", "(you)");

        // --- multiplayer / LAN ---
        p("Sukurti lobby", "Create lobby");
        p("Sukurti", "Create");
        p("Ieškoti hostų", "Find hosts");
        p("Ieškoti / atnaujinti", "Search / refresh");
        p("Rasti hostai", "Hosts found");
        p("Hostų nerasta. Kitame įrenginyje: Create → Sukurti lobby.",
          "No hosts found. On the other device: Create -> Create lobby.");
        p("Mano IP: ", "My IP: ");
        p("LAN Lobby — ", "LAN Lobby - ");
        p("Pasiruošti", "Ready");
        p("Ieškoma...", "Searching...");
        p("Ieškoma…", "Searching...");
        p("Slot ", "Slot ");

        // --- campaign ---
        p("Kampanija — pasirink epizodą:", "Campaign - choose an episode:");
        p("‹ Epizodai", "< Episodes");
        p("Žaisti misiją", "Play mission");
        p("Misija įveikta!", "Mission complete!");
        p("Kampanija baigta!", "Campaign complete!");
        p("Sveikiname — įveikei visą epizodą!", "Congratulations - you finished the episode!");
        p("Kita misija:\n", "Next mission:\n");
        p("(nėra aprašymo šiam žemėlapiui)", "(no description for this map)");
        p("Kraunama...", "Loading...");
        p("Kraunama…", "Loading...");
        p("misijų", "missions");

        // --- mods ---
        p("Visi", "All");
        p("Aktyvūs", "Active");
        p("Įdiegti", "Installed");
        p("Talpykloje", "In repository");
        p("Paieška...", "Search...");
        p("Paieška…", "Search...");
        p("+ .zip", "+ .zip");
        p("Modų talpyklos adresas", "Mod repository address");
        p("Talpyklos adresas…", "Repository address...");
        p("https://.../mods  (index.json vieta)", "https://.../mods  (index.json location)");
        p("Pasirink modą iš sąrašo kairėje.", "Pick a mod from the list on the left.");
        p("Aktyvuoti", "Activate");
        p("Išjungti", "Disable");
        p("Pašalinti", "Remove");
        p("Pašalinti modą?", "Remove mod?");
        p("Parsisiųsti", "Download");
        p("Atnaujinti", "Update");
        p("Priklausomybės: ", "Dependencies: ");
        p("Diegiamas modas: ", "Installing mod: ");
        p("Siunčiama…", "Downloading...");
        p("Siunčiama… ", "Downloading... ");
        p("Išskleidžiama…", "Extracting...");
        p("Ruošiamasi…", "Preparing...");
        p("Nepavyko pašalinti.", "Could not remove.");

        // --- replay ---
        p("+ Importuoti", "+ Import");
        p("Importuoti Replay", "Import replay");
        p("Pasirink replay iš sąrašo kairėje.", "Pick a replay from the list on the left.");
        p("ŽIŪRĖTI", "WATCH");
        p("Pašalinti replay?", "Remove replay?");
        p("(nėra replay failų)", "(no replay files)");
        p("(nėra atitinkančių paieškai)", "(nothing matches the search)");
        p("Antraštė nenuskaityta: ", "Header unreadable: ");
        p("Žaidimas prieš kompiuterį — jo atkurti negalima.",
          "This is a game against the computer - it cannot be replayed.");
        p("Trukmė: ", "Duration: ");

        // --- settings ---
        p("Kalba", "Language");
        p("Garsas", "Sound");
        p("Grafika", "Graphics");
        p("Valdymas", "Controls");
        p("Sąsaja", "Interface");
        p("Žaidimas", "Game");
        p("Duomenys", "Data");
        p("Apie", "About");
        p("Dev", "Dev");
        p("Garsumas: ", "Volume: ");
        p("Muzika: ", "Music: ");
        p("Raiška:", "Resolution:");
        p("Valdymas:", "Controls:");
        p("Sąsaja:", "Interface:");
        p("Rodyti QUEUE mygtuką", "Show the QUEUE button");
        p("Rodyti SHIFT mygtuką", "Show the SHIFT button");
        p("Klasikiniame HUD: komandų eilės ir pažymėjimo papildymo mygtukas.", "Classic HUD: the button that queues orders and adds to the selection.");
        p("Žaidimo HUD", "In-game HUD");
        p("HUD stilius", "HUD style");
        p("Paleidimo ekranas", "Start-up screen");
        p("Žaidimo pradžios paveikslas iš jūsų failų. HD — iš SC:R failų.", "The game's title picture from your files. HD: from the SC:R files.");
        p("Išjungtas", "Off");
        p("Lietimo HUD: komandų ratas po pirštu, dokas kampe, be juostos apačioje.", "Touch HUD: a command wheel under your finger, a dock in the corner, no bar at the bottom.");
        p("Klasikinis", "Classic");
        p("Lietimo", "Touch");
        p("Toliau — tik lietimo HUD.", "Below: the touch HUD only.");
        p("Laikysena", "Posture");
        p("Rankose: lankinis dokas prie nykščio, RATAS fiksatorius, kameros svirtelė.", "In hands: an arc dock at the thumb, the WHEEL latch, a camera stick.");
        p("Ant stalo / rašiklis", "On a desk / pen");
        p("Rankose", "In hands");
        p("Komandų dokas", "Command dock");
        p("Pagal laikyseną", "By posture");
        p("Lankas", "Arc");
        p("Rato delsa", "Wheel delay");
        p("Kiek laikyti pirštą, kol atsidaro ratas.", "How long to hold before the wheel opens.");
        p("Rašiklio mygtukas", "Pen button");
        p("Ratas: mygtukas + prisilietimas atidaro ratą iš karto.", "Wheel: button + touch opens the wheel at once.");
        p("Ratas", "Wheel");
        p("Fiksatorius ir svirtelė visur", "Latch and stick everywhere");
        p("RATAS fiksatorius ir kameros svirtelė ir laikysenoje „Ant stalo“.", "The WHEEL latch and the camera stick in the desk posture too.");
        p("Užrašai po doko ikonomis", "Labels under the dock icons");
        p("Kairiarankis", "Left-handed");
        p("Dokas kairėje, minimapas dešinėje.", "Dock on the left, minimap on the right.");
        p("HUD mastelis", "HUD scale");
        p("Vibracija", "Vibration");
        p("Rodyti visas 10 grupių", "Show all 10 groups");
        p("Išjungus — tik užpildytos grupės.", "Off: only the groups that hold units.");
        p("LEGACY UI (senas HUD)", "LEGACY UI (old HUD)");
        p("RGBA sprite'ai (HD pamatas)", "RGBA sprites (HD foundation)");
        p("Rodyti resursų kiekį žemėlapyje", "Show resource amounts on the map");
        p("Virš mineralų ir dujų rodomi likučiai bei dirbančių darbininkų skaičius.",
          "Remaining minerals/gas and worker saturation are shown over the field.");
        p("Žaidimo greitis:", "Game speed:");
        // The six speed names are VALUES, not captions: they come out of
        // SetupActivity.SPEED_NAMES and are pasted into the settings dropdown,
        // the LAN host cards and the join list. Wrapping the call sites was not
        // enough -- the strings themselves had to be translatable, or the
        // English UI kept saying "speed: Normalus".
        p("Lėtas", "Slowest");
        p("Lėtesnis", "Slower");
        p("Normalus", "Normal");
        p("Greitas", "Fast");
        p("Greitesnis", "Faster");
        p("Greičiausias", "Fastest");
        p("Pasiruošęs", "Ready");
        // v0.83.008 (S1.1): the native-code consent dialog. Added WITH the
        // feature, not after it -- the v0.83.001 lesson was that call sites
        // wrapped in S.t() still read Lithuanian when the table has no entry.
        p("Įdiegti vykdomą kodą?", "Install executable code?");
        p("Šiame mode yra vykdomo kodo (botų bibliotekų):",
          "This mod contains executable code (bot libraries):");
        p("Jos veiks su visomis šios programos teisėmis. Diegti jas tik jei pasitiki šaltiniu.",
          "They will run with all of this app's permissions. Install only if you trust the source.");
        p("Įdiegti", "Install");
        p("Praleisti", "Skip");
        p("botų bibliotekos praleistos (nepatvirtinta)", "bot libraries skipped (not approved)");
        // v0.83.009 (S1.2/S1.3): compatibility gate + exclusive-category swap.
        p("Nesuderinamas modas", "Incompatible mod");
        p("Modui reikia naujesnės programos versijos (versionCode ≥ ",
          "This mod needs a newer app version (versionCode >= ");
        p("Modas skirtas senesnei programos versijai (versionCode ≤ ",
          "This mod targets an older app version (versionCode <= ");
        p("Ta pati kategorija jau užimta", "That category is already taken");
        p("Šias kategorijas jau teikia kitas aktyvus modas:",
          "These categories are already provided by another active mod:");
        p("Variklis kiekvienai kategorijai naudoja tik VIENĄ modą. Pakeisti?",
          "The engine uses only ONE mod per category. Replace?");
        p("Pakeisti", "Replace");
        p("Atšaukti", "Cancel");
        p("Teikia: ", "Provides: ");
        // v0.83.010 (S1.5): repository address + token, integrity check.
        p("https://…/mods  (openbw-<versionCode>.json vieta)",
          "https://…/mods  (where openbw-<versionCode>.json lives)");
        p("Prieigos raktas (privačiai saugyklai; nebūtinas)",
          "Access token (for a private repository; optional)");
        p("Raktas lieka tik šiame įrenginyje. Indeksas imamas pagal versiją: openbw-",
          "The token stays on this device only. The index is fetched by version: openbw-");
        p("Išsaugoti + gauti", "Save + fetch");
        p("Tikrinama…", "Verifying…");
        p("kontrolinė suma nesutampa (failas sugadintas arba pakeistas)",
          "checksum mismatch (the file is damaged or was replaced)");
        p("privačiai saugyklai reikia prieigos rakto", "a private repository needs an access token");
        p("privatiems modams reikia prieigos kodo", "private mods need an access code");
        p("(nėra modų)", "(no mods)");
        p("prieigos kodas netinka", "the access code is not valid");
        p("Prieigos kodas (privatiems modams; nebūtinas)", "Access code (for private mods; optional)");
        p("Kodas lieka tik šiame įrenginyje.", "The code stays on this device only.");
        p("prieigos raktas atmestas", "access token rejected");
        p("Nerastas failas Release'e: ", "File not found in the release: ");
        p("(* — tik vienas modas kategorijai)", "(* — one mod per category)");
        p("⚠ Šiame mode yra vykdomo kodo (botų)", "⚠ This mod contains executable code (bots)");
        // v0.83.001: found by reading the Settings screen on the tablet in
        // English. Every one of these was already wrapped in S.t() -- the call
        // sites were fine, the TABLE was missing the entry, so they fell
        // through to Lithuanian -- the "coverage not proven" caveat from v0.80,
        // now with concrete holes filled.
        p("Rodyti saturaciją", "Show worker saturation");
        p("Kiek darbininkų dirba prie mineralų klasterio ir prie dujų.",
          "How many workers are on each mineral cluster and on gas.");
        p("Equipment ikonos pilkos", "Grey equipment icons");
        p("Nuspalvina Unit Display ekipuotės ikonas pilkai virš bet kokio ikonų pako. "
          + "Išjunk, jei naudoji spalvotų ikonų modą.",
          "Draws the Unit Display equipment icons grey on top of any icon pack. "
          + "Turn off if you use a colour icon mod.");
        p("Įrenginys: ", "Device: ");
        p("Reikalauja StarCraft: Brood War 1.16.1 MPQ (importuoti aukščiau).",
          "Requires StarCraft: Brood War 1.16.1 MPQ files (import above).");
        // open1161HD Faze 6: identity/credits body (one key -> translatable).
        p("Geriausias kosmoso RTS.\n"
        + "openBW+ (OpenBW-Android) atšaka.\n\n"
        + "Variklis: OpenBW — atviro kodo RTS variklis.\n\n"
        + "Naudoja jūsų paties žaidimo duomenų failus.",
          "The best space RTS.\n"
        + "A fork of openBW+ (OpenBW-Android).\n\n"
        + "Engine: OpenBW — the open-source RTS engine.\n\n"
        + "Uses your own game data files.");
        p("Atstatyti numatytus nustatymus", "Restore default settings");
        p("Atstatyti nustatymus?", "Restore settings?");
        p("Atstatyti", "Restore");
        p("Garsas, grafika, valdymas ir greitis grįš į numatytus. ",
          "Sound, graphics, controls and speed will return to their defaults. ");
        p("Atrakinti visas kampanijos misijas", "Unlock all campaign missions");
        p("Leidžia žaisti bet kurią misiją nebaigus ankstesnės.",
          "Lets you play any mission without finishing the previous one.");
        p("Išjungti karo rūką (Fog of War)", "Disable fog of war");
        p("Visas žemėlapis matomas. Determinizmui saugu - keičia tik vaizdą.",
          "The whole map is visible. Safe for determinism - presentation only.");
        p("Rodyti zoom mygtukus", "Show zoom buttons");
        p("Ekrane rodomi - / + mygtukai. Pinch-zoom veikia ir be jų.",
          "On-screen - / + buttons. Pinch-zoom works without them.");
        p("Atsitiktinė", "Random");
        // v0.81.003: found by reading the launcher on a tablet, not by grep --
        // these reach setText through string concatenation, so the original
        // pass that wrapped whole literals walked straight past them.
        p("Prisijungti", "Join");
        p("WiFi tinkle", "on WiFi");
        p("Tavo rasė:", "Your race:");
        p("Aktyvūs hostai (WiFi):", "Active hosts (WiFi):");
        p("greitis: ", "speed: ");
        p("  —  spausk, kad prisijungtum", "  —  tap to join");
        p("Neturi šio žemėlapio — negalima prisijungti", "You do not have this map — cannot join");
        p("(hostas)", "(host)");
        p("pasiruošę ", "ready ");
        p("pasiruošęs", "ready");
        p("AKTYVUS", "ACTIVE");
        p("įdiegtas", "installed");
        p("talpykloje", "in repository");
        p("Originalus AI (aiscript)", "Original AI (aiscript)");
        p("Terran — Rebel Yell (I epizodas)", "Terran — Rebel Yell (Episode I)");
        p("Zerg — Overmind (II epizodas)", "Zerg — Overmind (Episode II)");
        p("Protoss — The Fall (III epizodas)", "Protoss — The Fall (Episode III)");
        p("Triggerių pėdsakas į logą", "Trigger trace to the log");
        p("AI skriptų pėdsakas į logą", "AI script trace to the log");
        p("Praneša, kuriuos aiscript opcodes skriptai realiai pasiekia, bet variklis dar nevykdo.",
          "Reports which aiscript opcodes the scripts actually reach but the engine does not run yet.");
        p("Kampanijų diagnostikai: registracija ir sąlygų reikšmės į openbw-log.txt.",
          "Campaign diagnostics: registration and condition values into openbw-log.txt.");

        // --- MPQ import / setup gate ---
        p("Importuoti MPQ failus", "Import MPQ files");
        // open1161HD Faze 6 settings redesign.
        p("Pasirinkti SC:R aplanką", "Select SC:R folder");
        p("Grafika (HD):", "Graphics (HD):");
        p("Emissive švytėjimas", "Emissive glow");
        p("SC:R vienetų savišvytėjimas — ginklų blyksniai, psi, skydai, warp.",
          "SC:R unit self-glow — weapon flashes, psi, shields, warp.");
        p("Minkšti šešėliai", "Soft shadows");
        p("Švelnesni, permatomesni vienetų šešėliai (vietoj kietų juodų).",
          "Softer, more transparent unit shadows (instead of hard black).");
        p("Bloom (švytėjimo aureolė)", "Bloom (glow halo)");
        p("Vienetų šešėlių švelnumas. 0 = klasikinis kietas juodas; didesnis — šviesesnis.",
          "Unit shadow softness. 0 = classic hard black; higher = lighter.");
        p("GPU švytėjimo aureolės stiprumas aplink ryškias vietas. 0 = išjungta.",
          "GPU glow-halo strength around bright areas. 0 = off.");
        // open1161HD Faze 7+: sound submixes
        p("Garso efektai (SFX)", "Sound effects (SFX)");
        p("Mūšio, judesio ir aplinkos garsai", "Combat, movement and ambient sounds");
        p("Vienetų balsai", "Unit voices");
        p("Patvirtinimai ir atsakymai („taip, pone\")", "Acknowledgements and responses (\"yes sir\")");
        p("Sąsajos garsai", "Interface sounds");
        p("Pranešimai, mygtukai ir misijų balsai", "Notices, buttons and mission voice lines");
        p("Balsų dažnis", "Voice frequency");
        p("Kaip dažnai vienetai atsako pažymėjus (100 = kaip originale)",
          "How often units answer when selected (100 = original)");
        // open1161HD Faze 7+: interface controls
        p("Rodyti FPS", "Show FPS");
        p("Kadrai per sekundę viršutiniame kairiajame kampe.", "Frames per second in the top-left corner.");
        p("Rodyti APM", "Show APM");
        p("Veiksmai per minutę (actions per minute) po laikrodžiu.", "Actions per minute, below the clock.");
        p("Visada rodyti sveikatos juostas", "Always show health bars");
        p("Sveikatos juostos po VISAIS vienetais, ne tik pažeistais ar pažymėtais.",
          "Health bars under ALL units, not just damaged or selected ones.");
        p("Kameros greitis", "Camera speed");
        p("Krašto ir klaviatūros slinkimo greitis (100 = įprastas).",
          "Edge and keyboard scroll speed (100 = normal).");
        p("Atidaryti nustatymus", "Open settings");
        p("Kaip supakuoti ir talpinti modą", "How to package and host a mod");
        p("Modų talpykla", "Mod repository");
        p("Modų žemėlapiai", "Mod maps");
        p("Ne SC:R šaknis?", "Not the SC:R root?");
        p("Nepavyko atidaryti aplankų parinkiklio", "Could not open the folder picker");
        p("Nepavyko nustatyti aplanko kelio", "Could not resolve the folder path");
        p("Reikia „Visų failų prieigos“", "All-files access required");
        p("SC:R aplankas nustatytas (CASC įjungtas)", "SC:R folder set (CASC enabled)");
        p("Virš mineralų ir dujų rodomas likutis.", "Shown above minerals and gas.");
        p("SC:R rasta — HD asset'ai įjungti.", "SC:R found — HD assets enabled.");
        p("SC:R aplankas nepasirinktas — tik SD / HD2.", "No SC:R folder selected — SD / HD2 only.");
        p("SD — klasikinė 8-bit grafika (be SC:R). HD2 — HD, pusės raiškos "
                + "(rekomenduojama). FULL HD — pilna HD raiška (reikia SC:R + galingos planšetės).",
          "SD — classic 8-bit graphics (no SC:R). HD2 — HD at half resolution "
                + "(recommended). FULL HD — full HD resolution (needs SC:R + a strong tablet).");
        p("Pasirinkti MPQ failus", "Choose MPQ files");
        p("Trūksta MPQ: ", "Missing MPQ: ");
        p("MPQ failai rasti", "MPQ files found");
        p("Būtini failai", "Required files");
        p("Papildomi failai (kampanijoms ir muzikai)", "Optional files (campaigns and music)");
        p("Be jo kampanijos ir muzika neveiks", "Without it campaigns and music will not work");
        p("Šiai programai reikia TAVO turimos StarCraft: Brood War 1.16.1 kopijos ",
          "This app needs YOUR OWN copy of StarCraft: Brood War 1.16.1's ");
        p("duomenų failų. Jie nėra platinami su programa ir nėra parsisiunčiami — "
          + "juos importuoji pats iš savo įdiegto žaidimo.",
          "data files. They are not distributed with the app and are not downloaded - "
          + "you import them yourself from your installed game.");
        p("Failai tikrinami importuojant: MPQ parašas ir dydis. ",
          "Files are checked on import: MPQ signature and size. ");
        p("Remastered (CASC) formato failai netinka — reikia būtent 1.16.1.",
          "Remastered (CASC) files will not do - it must be 1.16.1.");
        p("Pirma importuokite MPQ failus.", "Import the MPQ files first.");
        p("Importuota su pastabomis", "Imported with warnings");
        p("Kopijuojama:\n", "Copying:\n");
        p("Nukopijuota failų: ", "Files copied: ");

        // --- strings built by concatenation, wrapped in a second pass ---
        p("    Pergalės  Z ", "    Wins  Z ");
        p("Be AI (tuščias, testavimui)", "No AI (empty, for testing)");
        p("  (tu)", "  (you)");
        p("(nėra modų — importuok .zip arba nustatyk talpyklos adresą)",
          "(no mods - import a .zip or set a repository address)");
        p("(nėra aprašymo)", "(no description)");
        p("Importuoti", "Imported");
        p("Klasikiniai (SC) — 2–4 žaidėjų", "Classic (SC) - 2-4 players");
        p("Klasikiniai (SC) — 5–8 žaidėjų", "Classic (SC) - 5-8 players");
        p("(nėra ", "(no ");
        p("StarCraft", "StarCraft");
        p("Brood War", "Brood War");
        p("Pilna", "Full");
        p("Pusė", "Half");
        p("Trečdalis", "Third");
        p("MPQ failai rasti. Muzikai papildomai importuokite StarCraft.mpq ir BroodWar.mpq (CD archyvai).",
          "MPQ files found. For music, also import StarCraft.mpq and BroodWar.mpq (the CD archives).");

        // --- score screen ---
        p("MISIJA ĮVYKDYTA", "MISSION ACCOMPLISHED");
        p("MISIJA NEPAVYKO", "MISSION FAILED");
        p("Žaidėjas", "Player");
        p("Unitai", "Units");
        p("Pastatai", "Structures");
        p("Sunaikinta", "Destroyed");
        p("Prarasta", "Lost");
        p("Mineralai", "Minerals");
        p("Dujos", "Gas");
        p("Taškai", "Score");
        p("Tu", "You");
        p("Išsaugoti replay", "Save replay");
        p("Išsaugota: ", "Saved: ");
        p("Nepavyko išsaugoti replay.", "Could not save the replay.");
        // v0.88.x translation-completeness pass (discrete user-facing strings).
        p("Lėtas", "Slow");
        p("Lėtesnis", "Slower");
        p("Normalus", "Normal");
        p("Greitas", "Fast");
        p("Greitesnis", "Faster");
        p("Greičiausias", "Fastest");
        p("Pasirinkite žemėlapį.", "Select a map.");
        p("Žemėlapis nerastas.", "Map not found.");
        p("REPLAY tabe pasirinkite replay failą.", "Select a replay file in the REPLAY tab.");
        p("Multiplayer: naudokite Create (Sukurti lobby) arba Join (pasirinkite hostą).",
          "Multiplayer: use Create (host a lobby) or Join (pick a host).");
        p("KAMPANIJA tabe pasirinkite misiją.", "Select a mission in the CAMPAIGN tab.");
        p("Įveskite host IP.", "Enter the host IP.");
        p("(nė vienas modas neatitinka filtro)", "(no mod matches the filter)");
        p("FULL HD buvo per lėtas šiam įrenginiui — grafika perjungta į HD2.",
          "FULL HD was too slow for this device — graphics switched to HD2.");
        // --- i18n coverage pass (tools/i18n_scan.py): SCBW2 QoL + save/misc ---
        p("SCBW2 (QoL namų taisyklės):", "SCBW2 (QoL house rules):");
        p("Group rally — visiems pažymėtiems pastatams", "Group rally — to all selected buildings");
        p("MBS — gamyba iš visų pažymėtų pastatų", "MBS — produce from all selected buildings");
        p("Auto-worker — automatiškai gamina darbininkus", "Auto-worker — auto-produces workers");
        p("Smart-cast — burtą meta tik geriausios energijos casteris", "Smart-cast — only the best-energy caster casts");
        p("Enhanced unit selection — >12 unitų (iki 72), 1/2/3 sub-kortelės", "Enhanced unit selection — >12 units (up to 72), 1/2/3 sub-cards");
        p("Kelių pastatų žymėjimas (sim) — pastatai vienoje selekcijoje", "Multi-building select (sim) — buildings share one selection");
        p("IŠSAUGOTI", "SAVE");
        p("Išsaugoti žaidimai", "Saved games");
        p("(nėra išsaugotų žaidimų)", "(no saved games)");
        p("Tęsti žaidimą?", "Resume game?");
        p("AI", "AI");
        p("Player", "Player");
        p("kadrų", "frames");
        p("kontrolinė suma nesutampa", "checksum mismatch");
        p("Įtaisyti botai:", "Bundled bots:");
        // concatenated settings-description fragments (keep trailing space / \n so the
        // pieces still join after translation)
        p("SD — klasikinė 8-bit grafika (be SC:R). HD2 — HD, pusės raiškos ",
          "SD — classic 8-bit graphics (no SC:R). HD2 — HD, half-resolution ");
        p("Nuspalvina Unit Display ekipuotės ikonas pilkai virš bet kokio ikonų pako. ",
          "Tints the Unit Display equipment icons grey over any icon pack. ");
        p("Adresas ir raktas lieka tik šiame įrenginyje. Įvesk vieną kartą — ",
          "The address and key stay only on this device. Enter once — ");
        p("Geriausias kosmoso RTS.\n", "The best space RTS.\n");
        p("SC:R duomenys skaitomi vietoje iš tavo aplanko (dešimtys GB, ",
          "SC:R data is read in place from your folder (tens of GB, ");
        p("Aplanke\n", "In folder\n");
        p("\nnerasta „.build.info“ ir „data/data“. ",
          "\n'.build.info' and 'data/data' not found. ");
        p("Jos veiks su visomis šios programos teisėmis. ",
          "They will run with all of this app's permissions. ");

        // --- i18n audit 2026-08-11: strings that were shown untranslated (LT) to
        // English users because they had no EN_MAP entry. Keys match the S.t() calls
        // exactly, including trailing spaces / \n. ---
        p("Bakstelėjimas = komanda", "Tap to command");
        p("Pažymėjus vienetus, bakstelėjimas žemėje/priešą duoda komandą (be ilgo paspaudimo). Savo vieneto bakstelėjimas vis tiek pažymi.",
          "With units selected, a tap on the ground/an enemy issues an order (no long-press). Tapping your own unit still selects it.");
        p("LAN sklandumas (interpoliacija)", "LAN smoothing (interpolation)");
        p("Tinklo žaidime sklandžiai interpoliuoja vienetų judesį tarp lockstep kadrų (mažina joinerio trūkčiojimą). Tik tinklo žaidimams; grynai vizualu.",
          "In a network game, smoothly interpolates unit motion between lockstep frames (reduces joiner stutter). Network games only; purely visual.");
        p("Vieno žaidėjo", "Single player");
        p("Spalva", "Colour");
        p("Kampanija", "Campaign");
        p("Dabartinė misija", "Current mission");
        p("Misijos aprašymas", "Mission description");
        p("PRADĖTI MISIJĄ", "START MISSION");
        p("Pakartoti misiją", "Repeat mission");
        p("Įveikta", "Completed");
        p("Prieinama", "Available");
        p("užimta", "taken");
        p("Auto-worker — automatiškai gamina darbininkus (tik SP)",
          "Auto-worker — automatically produces workers (SP only)");
        p("Ryškumo filtras (Sharpen)", "Sharpen filter");
        p("Sharpen stiprumas", "Sharpen strength");
        p("Kiek stipriai ryškinti. Per stipriai — atsiranda triukšmas ir aureolės aplink kraštus.",
          "Sharpen strength. Too high adds noise and halos around edges.");
        p("GPU ryškinimas — HD2 grafika atrodo aštriau be FULL HD kainos. Auto: įjungta ",
          "GPU sharpen — HD2 graphics look crisper without the FULL HD cost. Auto: on ");
        p("Kinematinis atspalvis", "Cinematic color grade");
        p("Švelnus spalvų koregavimas (kontrastas, sodrumas, šiluma) — kino atspalvis. 0 = išjungta.",
          "Gentle color grade (contrast, saturation, warmth) — cinematic tint. 0 = off.");
        p("Dinaminės šviesos", "Dynamic lights");
        p("Dinaminių šviesų ryškumas ir spindulys.", "Dynamic light brightness and radius.");
        p("Šviesų stiprumas", "Light strength");
        p("Kryptinis saulės šešėlis", "Directional sun shadow");
        p("Šešėlio ilgis", "Shadow length");
        p("Kiek stipriai pakreipti/pailginti kryptinį šešėlį. 0 = beveik kaip keptas.",
          "How much to slant/lengthen the directional shadow. 0 = almost baked in.");
        p("Pastatai/vienetai meta kryptinį šešėlį (pakreiptas silueto projektavimas) ",
          "Buildings/units cast a directional shadow (slanted silhouette projection) ");
        p("Burtai, ginklų ugnis ir sprogimai apšviečia aplinką (SC:R švytėjimo sluoksnis). ",
          "Spells, weapon fire and explosions light the surroundings (SC:R glow layer). ");
        p("KAIP PASILEISTI SAVO MODŲ SĄRAŠĄ IŠ SAVO GIT\n",
          "HOW TO LOAD YOUR OWN MOD LIST FROM YOUR GIT\n");
    }
}
