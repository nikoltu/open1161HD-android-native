package org.openbw.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Launcher / main menu. StarCraft assets stay on the user's device: they
 * pick their own 1.16.1 MPQs, replays and maps via the system file picker
 * and they are copied into this app's private storage where the native side
 * (SDL_AndroidGetExternalStoragePath) reads them. From here the user selects
 * a map (melee, optionally over LAN) or a replay and presses START GAME.
 */
public class SetupActivity extends Activity {

    private static final int REQ_PICK_MPQ = 1;
    private static final int REQ_PICK_REPLAY = 2;
    private static final int REQ_PICK_MAP = 3;
    private static final int REQ_PICK_BOT = 4;
    private static final int REQ_PICK_MOD = 5; // v0.26: mod .zip packages
    private static final int REQ_PICK_SCR = 6; // open1161hd: SC:R install folder (CASC source)
    private TextView scrStatus;                 // Faze 6: green "SC:R OK" / red "no folder"
    private View scrIndicator;                  // green when an SC:R folder is set

    private static final String[] MPQ_NAMES = {"StarDat.mpq", "BrooDat.mpq", "Patch_rt.mpq"};
    private static final long[] MPQ_MIN = {55_000_000L, 18_000_000L, 400_000L};
    private static final long[] MPQ_MAX = {80_000_000L, 32_000_000L, 4_000_000L};
    private static final int MPQ_MAGIC = 0x1a51504d;

    // v0.81: built per call, not a static final array. A static array of
    // user-visible strings is evaluated at class load, which happens before
    // onCreate sets the language -- so a translated one would freeze whatever
    // language was active when the class first loaded. Race names are the only
    // place this bit us ("Atsitiktinė" stayed Lithuanian in English mode).
    private static String[] raceNames() {
        return new String[]{"Zerg", "Terran", "Protoss", S.t("Atsitiktinė")};
    }
    private static final int[] RACE_VALUES = {0, 1, 2, 5};


    // v0.26 tabs. START is tab-aware so a selected campaign no longer hijacks a
    // melee launch from the ŽAISTI tab (and vice versa).
    private static final int TAB_SINGLE = 0, TAB_MULTI = 1, TAB_KAMPANIJA = 2,
            TAB_MODAI = 3, TAB_REPLAY = 4, TAB_SAVE = 5, TAB_NUSTATYMAI = 6;
    private int activeTab = TAB_SINGLE;

    private static final int COL_CARD = Color.rgb(34, 36, 48);
    private static final int COL_CARD_SEL = Color.rgb(44, 74, 92);
    private static final int COL_MAP = Color.rgb(30, 42, 34);
    private static final int COL_MAP_SEL = Color.rgb(46, 82, 58);

    private final Handler ui = new Handler(Looper.getMainLooper());

    // v0.53: map-preview minimap generator (libopenbw_minimap.so - SDL-free).
    // Loaded lazily/optionally so the launcher still works if the lib is absent.
    private static boolean minimapLibOk = false;
    static {
        try { System.loadLibrary("openbw_minimap"); minimapLibOk = true; }
        catch (Throwable t) { minimapLibOk = false; }
    }
    // Return null on success (PNG written to outPng), or an error string.
    private static native String nativeGenMinimap(String gameDir, String mapFile, String outPng);
    private static native String nativeGenMinimapInternal(String gameDir, String mapPath, String outPng);
    // v0.56: scenario name + description ("name\ndescription"), or null on fail.
    private static native String nativeMapInfo(String mapFile);
    private static native String nativeMapInfoInternal(String gameDir, String mapPath, String scratch);
    private final java.util.concurrent.ExecutorService minimapExec =
            java.util.concurrent.Executors.newSingleThreadExecutor();

    private View mpqIndicator;
    private TextView mpqStatus;
    private LinearLayout mapList;
    private LinearLayout replayList;
    private EditText joinIpField;
    private View startButton;             // the action bar (primary action of the current screen)
    // Launcher redesign F6: the shell -- a left rail replaces the tab bar.
    private Kit kit;
    private LinearLayout[] railEntries;   // index = TAB_*
    private TextView screenTitle, screenSub, actionSummary;
    private View drawerScrim;             // compact (< 600 dp) layout: the rail is a drawer
    private ScrollView pageScroll;        // the panel the pages live in
    private TextView secondaryAction;     // action bar: the screen's secondary action (SP: import a map)
    // Single player screen (F7): maps | preview | players
    private TextView mapCountLabel;
    private PreviewPane spPane, mpPane, joinPane, lobbyPane;
    private TextView mpMapCountLabel, mpHostsLabel;
    private TextView tertiaryAction, primaryAction;
    private LinearLayout oppBox;
    private static final Object MAP_ROW = new Object();   // tag: a map list row (flat, not a card)

    private File selectedMap;
    private File selectedReplay;
    private View selectedCard;
    private int selectedCardDefaultColor;

    // v1.0.9: the game's own title picture for a few seconds before the launcher, once
    // per start of the app (also right after the first-run file selection). The picture
    // comes from the user's files: the original title screen (glue\\title\\title.pcx) or
    // SC:R's (glue/title/title.dds) -- Settings > Interface; black behind it; a tap skips.
    private static boolean splashShown;
    private static final int SPLASH_MS = 3500;
    private void showStartSplash(final FrameLayout root, final File gameDir, Bundle savedInstanceState) {
        // adb TEST hook: `--ez splash true` shows it again in a running process
        final boolean force = getIntent().getBooleanExtra("splash", false);
        if (!force && splashShown) return;   // recreate() (language, results) stays in the process: no second showing
        // Coming back from a game after Android dropped the launcher process is not a start
        // of the app: startActivity() below leaves a mark, taken here.
        if (!force && prefs().getBoolean("inGame", false)) { prefs().edit().putBoolean("inGame", false).apply(); splashShown = true; return; }
        splashShown = true;
        final String mode = prefs().getString("startSplash", "classic");
        if ("off".equals(mode)) return;
        final FrameLayout cover = new FrameLayout(this);
        cover.setBackgroundColor(Color.BLACK);
        cover.setClickable(true);              // nothing behind it reacts while it is up
        final ImageView pic = new ImageView(this);
        pic.setScaleType(ImageView.ScaleType.FIT_CENTER);
        cover.addView(pic, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(cover, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        final Runnable close = new Runnable() {
            boolean done;
            @Override public void run() {
                if (done) return;
                done = true;
                cover.animate().alpha(0f).setDuration(300).withEndAction(() -> {
                    root.removeView(cover);
                    pic.setImageDrawable(null);
                }).start();
            }
        };
        cover.setOnClickListener(v -> close.run());
        final String cascDir = prefs().getString("scrCascDir", "");
        final boolean full = "full".equals(prefs().getString("gfxTier", "hd2"));
        new Thread(() -> {
            Bitmap b = null;
            if ("hd".equals(mode)) b = PortraitStore.titleHd(cascDir, full);
            if (b == null) b = PortraitStore.titleClassic(gameDir);
            final Bitmap bmp = b;
            ui.post(() -> {
                if (bmp == null) { root.removeView(cover); return; }   // no picture: no black wait either
                pic.setImageBitmap(bmp);
                // The clock starts with the first frame that shows the picture -- on a cold
                // start the window is not drawn yet while the launcher behind it is built.
                pic.getViewTreeObserver().addOnPreDrawListener(new android.view.ViewTreeObserver.OnPreDrawListener() {
                    @Override public boolean onPreDraw() {
                        pic.getViewTreeObserver().removeOnPreDrawListener(this);
                        ui.postDelayed(close, SPLASH_MS);
                        return true;
                    }
                });
                pic.invalidate();
            });
        }, "splash").start();
    }

    @Override
    public void startActivity(Intent intent) {
        if (intent != null && intent.getComponent() != null && OpenBWActivity.class.getName().equals(intent.getComponent().getClassName()))
            prefs().edit().putBoolean("inGame", true).commit();
        super.startActivity(intent);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        S.setLanguage(prefs().getString("lang", S.EN));

        // v0.70 item 17: without the game's own data files there is nothing to
        // launch, so the first screen is the import gate rather than a launcher
        // full of empty lists. It also states plainly that the user must supply
        // their own legal 1.16.1 copy - nothing is bundled or downloaded.
        // Launcher redesign F6: the gate is the first-run window with two cards (Brood
        // War: required, SC:Remastered: optional). It stays up until the player presses
        // Continue ("firstRunPending"), so SC:R can be picked right there as well.
        if (!mpqMissing().isEmpty()) prefs().edit().putBoolean("firstRunPending", true).apply();
        // adb TEST hook (`am start -n .../.SetupActivity --ei gatePreview N`): show the
        // first-run window although the files are there. 1 = with the real file state,
        // 2 = drawn as a clean install sees it. Nothing on disk is touched.
        gatePreview = getIntent().getIntExtra("gatePreview", 0);
        if (prefs().getBoolean("firstRunPending", false) || gatePreview != 0) { gateMode = true; setContentView(buildSetupGate()); return; }
        gateMode = false;

        // Built-in opponents ship inside the signed APK (assets/bots/) and are
        // extracted to files/bots/ so the launcher always has working bots with no
        // download. Runs once per app version.
        extractBundledBots();

        // Faze 4.5: FULL HD auto-fallback. The engine drops this marker (in the
        // app's internal files dir, == SDL_AndroidGetInternalStoragePath) when
        // FULL HD could not sustain a playable frame rate on this device. Honor it
        // by switching the graphics tier to HD2 for this and all future launches,
        // then remove the marker so it fires once.
        try {
            File dg = new File(getFilesDir(), "gfx_auto_hd2");
            if (dg.exists()) {
                dg.delete();
                if ("full".equals(prefs().getString("gfxTier", "hd2"))) {
                    prefs().edit().putString("gfxTier", "hd2").apply();
                    Toast.makeText(this, S.t("FULL HD buvo per lėtas šiam įrenginiui — grafika perjungta į HD2."), Toast.LENGTH_LONG).show();
                }
            }
        } catch (Exception e) {}

        // Launcher redesign F6: the shell. A left
        // rail (profile block, the seven screens in three groups, file status) replaces
        // the tab bar; the content column is header + scrolling page + one action bar.
        // Single Activity with show/hide page containers as before -- pages[0..6] and
        // every prefs/flow are untouched, only their frame changed. Under 600 dp wide
        // the rail becomes a drawer behind a menu button.
        kit = new Kit(this);
        final boolean wide = getResources().getConfiguration().screenWidthDp >= 600;
        final File shellGameDir = getExternalFilesDir(null);
        // HD portraits from the user's SC:R folder when the graphics tier is HD (F5).
        PortraitStore.configureHd(prefs().getString("scrCascDir", ""), prefs().getString("gfxTier", "hd2"), getCacheDir());
        // Menu music and interface sounds from the user's own archives (F9).
        MenuAudio.prepare(this, shellGameDir, prefs());
        FrameLayout shellRoot = new FrameLayout(this);
        shellRoot.setBackgroundColor(0xFF05080A);
        // Backdrop: the main-menu space scene from the user's own StarDat (dimmed).
        final ImageView backdrop = new ImageView(this);
        backdrop.setScaleType(ImageView.ScaleType.CENTER_CROP);
        backdrop.setAlpha(0.5f);
        shellRoot.addView(backdrop, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        Bitmap bgBmp = PortraitStore.cachedBackground(shellGameDir);
        if (bgBmp != null) backdrop.setImageBitmap(bgBmp);
        else new Thread(() -> {
            final Bitmap b = PortraitStore.background(shellGameDir);
            if (b != null) ui.post(() -> backdrop.setImageBitmap(b));
        }, "backdrop").start();

        LinearLayout outer = new LinearLayout(this);
        outer.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        int pad = dp(wide ? 12 : 8);
        outer.setPadding(pad, pad, pad, pad);
        shellRoot.addView(outer, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(shellRoot);
        showStartSplash(shellRoot, shellGameDir, savedInstanceState);

        // Fields, not locals: selectTab() switches screens from outside onCreate.
        pages = new LinearLayout[7];
        railEntries = new LinearLayout[7];
        View rail = buildRail(wide);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);

        screenTitle = kit.text("", Color.WHITE, 22);
        screenTitle.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        screenTitle.setLetterSpacing(0.04f);
        screenSub = kit.text("", Kit.TEXT2, 13);
        screenSub.setPadding(dp(14), 0, 0, dp(2));
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.BOTTOM);
        if (wide) {
            outer.addView(rail, new LinearLayout.LayoutParams(dp(236), ViewGroup.LayoutParams.MATCH_PARENT));
            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
            clp.leftMargin = dp(12);
            outer.addView(content, clp);
            head.setPadding(dp(4), 0, dp(4), dp(8));
        } else {
            // compact: menu button in the header, the rail slides in over the page
            TextView menu = kit.button("\u2630", 0, () -> { if (drawerScrim != null) drawerScrim.setVisibility(View.VISIBLE); });
            LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(dp(48), dp(48));
            mlp.rightMargin = dp(10);
            head.addView(menu, mlp);
            head.setGravity(Gravity.CENTER_VERTICAL);
            head.setPadding(0, 0, 0, dp(6));
            outer.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            FrameLayout scrim = new FrameLayout(this);
            scrim.setBackgroundColor(0xB8000000);
            scrim.setOnClickListener(v -> scrim.setVisibility(View.GONE));
            rail.setClickable(true);
            scrim.addView(rail, new FrameLayout.LayoutParams(dp(260), ViewGroup.LayoutParams.MATCH_PARENT));
            scrim.setVisibility(View.GONE);
            shellRoot.addView(scrim, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            drawerScrim = scrim;
        }
        head.addView(screenTitle);
        head.addView(screenSub);
        content.addView(head);

        ScrollView scroll = new ScrollView(this);
        pageScroll = scroll;
        scroll.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        scroll.setBackground(kit.box(Kit.PANEL, Kit.LINE));
        scroll.setPadding(dp(12), dp(8), dp(12), dp(8));
        LinearLayout pageHolder = new LinearLayout(this);
        pageHolder.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(pageHolder);
        content.addView(scroll);
        for (int t = 0; t < pages.length; t++) {
            pages[t] = new LinearLayout(this);
            pages[t].setOrientation(LinearLayout.VERTICAL);
            pages[t].setVisibility(t == 0 ? View.VISIBLE : View.GONE);
            pageHolder.addView(pages[t]);
        }

        // ---- SINGLE PLAYER (0): two columns — controls left, map list right ----
        buildSinglePlayerPage(pages[TAB_SINGLE]);

        // ---- MULTIPLAYER / LAN (1) ----
        buildMultiplayerPage(pages[TAB_MULTI]);

        // ---- KAMPANIJA (2) ----
        addCampaignRow(pages[TAB_KAMPANIJA]);

        // ---- MODAI (3): import/store + activation checkboxes ----
        buildModPage(pages[TAB_MODAI]);

        // ---- REPLAY (4): master-detail, same shape as the MOD tab ----
        buildReplayPage(pages[TAB_REPLAY]);

        // ---- IŠSAUGOTI (5): resumable disk saves, own tab (v0.86.001) ----
        buildSavePage(pages[TAB_SAVE]);

        // ---- NUSTATYMAI (5): data + language on one row, then settings ----
        // Faze 6 redesign: Import MPQ, Select SC:R and the language dropdown share
        // the top row (language right-aligned); the two status lines sit below in
        // green (ok) / red (missing). CASC turns on automatically when an SC:R
        // folder is chosen -- there is no separate toggle.
        buildSettingsPage(pages[TAB_NUSTATYMAI]);

        // The action bar: ONE primary action per screen, with a one-line summary on its
        // left. Single player owns it today; the other screens still carry their own
        // buttons inside the page (they move here screen by screen, phase F7).
        LinearLayout bottomBar = new LinearLayout(this);
        bottomBar.setOrientation(LinearLayout.HORIZONTAL);
        bottomBar.setGravity(Gravity.CENTER_VERTICAL);
        bottomBar.setPadding(dp(4), dp(10), 0, 0);
        actionSummary = kit.text("", Kit.TEXT2, 13);
        actionSummary.setSingleLine(true);
        actionSummary.setEllipsize(android.text.TextUtils.TruncateAt.END);
        bottomBar.addView(actionSummary, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        tertiaryAction = kit.button("", 0, () -> {});
        secondaryAction = kit.button("", 0, () -> {});
        primaryAction = kit.button("", 1, () -> {});
        primaryAction.setMinWidth(dp(220));
        primaryAction.setMinimumWidth(dp(220));
        for (TextView b : new TextView[]{tertiaryAction, secondaryAction, primaryAction}) {
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(56));
            blp.leftMargin = dp(12);
            bottomBar.addView(b, blp);
        }
        content.addView(bottomBar);
        startButton = bottomBar;
        selectTab(TAB_SINGLE);

        refresh();

        // v0.79 item 1: coming back from a campaign mission, land on that
        // episode's mission list (with its ticks updated) instead of Single
        // Player. Also runs from onResume for the survived-activity case.
        returnToEpisodeIfPending();
        returnToReplayIfPending();
        if (restoreTab >= 0) { int rt = restoreTab; restoreTab = -1; selectTab(rt); }
    }
    private static int restoreTab = -1;      // screen to reopen after recreate() (language / reset)
    private static int settingsTab = 0;      // the Settings sub-tab last shown

    @Override
    protected void onPause() {
        super.onPause();
        MenuAudio.pause();     // a game starts or the app goes to the background
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (prefs().getBoolean("inGame", false)) prefs().edit().putBoolean("inGame", false).apply();
        if (!gateMode) MenuAudio.resume();
        // v0.31: fold a finished melee/LAN game's result into profile stats.
        if (consumeMeleeResult()) { recreate(); return; }
        // v0.69 item 14: after-action score screen. Shown before anything else
        // so it is the first thing the player sees coming out of a match; it
        // owns the whole window (not a tab) and is dismissed with Return.
        if (!gateMode && showScoreScreenIfPending()) return;
        // v0.26 campaign progression: the native side leaves the next mission
        // dir in campaign-next.txt after a campaign victory.
        try {
            java.io.File f = new java.io.File(getExternalFilesDir(null), "campaign-next.txt");
            if (f.isFile()) {
                byte[] b = new byte[(int) f.length()];
                java.io.FileInputStream in = new java.io.FileInputStream(f);
                int n = in.read(b);
                in.close();
                f.delete();
                String next = n > 0 ? new String(b, 0, n).trim() : "";
                // Record the just-beaten mission for the picker's checkmarks.
                String prev = getCampaignDir();
                if (!prev.isEmpty()) {
                    java.util.Set<String> done = new java.util.HashSet<>(getCampaignDone());
                    done.add(prev);
                    setCampaignDone(done);
                }
                if (next.isEmpty()) {
                    setCampaignDir("");
                    new AlertDialog.Builder(this)
                            .setTitle(S.t("Kampanija baigta!"))
                            .setMessage(S.t("Sveikiname — įveikei visą epizodą!"))
                            .setPositiveButton(S.t("Gerai"), (d, w) -> recreate())
                            .setCancelable(false)
                            .show();
                } else {
                    // v0.33 seamless progression: offer to continue straight to the
                    // next mission instead of making the player re-navigate + START.
                    setCampaignDir(next);
                    String nextName = campaignDisplayName(next);
                    new AlertDialog.Builder(this)
                            .setTitle(S.t("Misija įveikta!"))
                            .setMessage(S.t("Kita misija:\n") + nextName)
                            .setPositiveButton(S.t("Tęsti"), (d, w) -> launchCampaignMission(next))
                            .setNegativeButton(S.t("Į meniu"), (d, w) -> recreate())
                            .setCancelable(false)
                            .show();
                }
                return;
            }
        } catch (Exception ignored) {
        }
        // v0.83.015: if we came back from a campaign mission and the activity
        // was NOT recreated (onCreate did not run -- the S8U case), land on the
        // episode's mission list instead of whatever tab was last shown.
        if (returnToEpisodeIfPending()) return;
        refresh();
        returnToReplayIfPending();
    }

    private LinearLayout listColumn(String title, LinearLayout list) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        // Full width: this column now sits alone in a vertical tab page (v0.26
        // tabs). A width-0 + weight layout would collapse it to zero width.
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = dp(4);
        lp.rightMargin = dp(4);
        col.setLayoutParams(lp);

        TextView t = new TextView(this);
        t.setText(title);
        t.setTextColor(Color.LTGRAY);
        t.setPadding(0, dp(8), 0, dp(4));
        col.addView(t);

        list.setOrientation(LinearLayout.VERTICAL);
        ScrollView sv = new ScrollView(this);
        sv.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(300)));
        // This ScrollView is nested inside the page's outer ScrollView; without
        // this the parent intercepts the drag and the inner list won't scroll.
        sv.setNestedScrollingEnabled(true);
        sv.setOnTouchListener((v, event) -> {
            v.getParent().requestDisallowInterceptTouchEvent(true);
            return false;
        });
        sv.addView(list);
        col.addView(sv);
        return col;
    }

    // v0.43 Single Player page (#5): left column = controls, right = map list.
    private LinearLayout mapListMp;       // MULTIPLAYER Create map list (mirrors mapList)
    private LinearLayout playerRacesBox;  // SP per-player [name][race] rows
    private int mpSub = 0;                // MULTIPLAYER sub-screen: 0 = Create, 1 = Join
    private LinearLayout mpCreateBox, mpJoinBox, mpJoinTools, mpRaceCreate, mpRaceJoin;
    private LanLobby.HostInfo mpSelHost;  // Join: the host picked in the list
    private View mpSelHostCard;
    private String lobbyPreviewMap = "";

    // The game speeds, in one place. SPEED_NAMES holds the Lithuanian keys (the
    // S table is keyed by the original text); every DISPLAY goes through S.t().
    // Previously the names were duplicated here and in the Settings dropdown as
    // bare literals, so the English UI showed "speed: Normalus" on the LAN host
    // cards -- verified on both tablets.
    static final int[] SPEED_PCT = {50, 75, 100, 150, 200, 300};
    static final String[] SPEED_NAMES =
            {S.t("Lėtas"), S.t("Lėtesnis"), S.t("Normalus"), S.t("Greitas"), S.t("Greitesnis"), S.t("Greičiausias")};

    // Maps a game-speed percent to its Settings label (for the host-scan card).
    private String gameSpeedName(String pctStr) {
        int pct;
        try { pct = Integer.parseInt(pctStr); } catch (Exception e) { return pctStr + "%"; }
        for (int k = 0; k < SPEED_PCT.length; k++)
            if (SPEED_PCT[k] == pct) return S.t(SPEED_NAMES[k]);
        return pct + "%";
    }


    // v1.0.22: all 16 rows of the game's own player colour table (game/tunit.pcx, index ==
    // engine player.color). RGB measured from the table through the game palette (the
    // brightest shade of each row). Row 14 is a byte-for-byte copy of row 9, so the picker
    // leaves it out: 15 different colours.
    static final int[] PLAYER_COLORS = {
        Color.rgb(0xF4,0x04,0x04), Color.rgb(0x0C,0x48,0xCC), Color.rgb(0x2C,0xB4,0x94),
        Color.rgb(0x88,0x40,0x9C), Color.rgb(0xF8,0x8C,0x14), Color.rgb(0x70,0x30,0x14),
        Color.rgb(0xCC,0xE0,0xD0), Color.rgb(0xFC,0xFC,0x38),
        Color.rgb(0x08,0x80,0x08), Color.rgb(0xFC,0xFC,0x7C), Color.rgb(0xEC,0xC4,0xB0),
        Color.rgb(0x40,0x68,0xD4), Color.rgb(0x74,0xA4,0x7C), Color.rgb(0x90,0x90,0xB8),
        Color.rgb(0xFC,0xFC,0x7C), Color.rgb(0x00,0xE4,0xFC) };
    static final String[] PLAYER_COLOR_NAMES = { "Red","Blue","Teal","Purple","Orange","Brown","White","Yellow",
        "Green","Pale Yellow","Tan","Azure","Pale Green","Blue Grey","Pale Yellow","Cyan" };
    static final int PLAYER_COLOR_DUPLICATE = 14;

    // A row of tappable colour swatches. `takenMask` bit N set = colour N is already
    // used by another player (dimmed + non-selectable); -1 = nothing taken (SP). The
    // chosen index is stored in pref `key`; onPick (nullable) fires after a change so a
    // lobby can broadcast it. Returns the row so callers can rebuild it on lobby updates.

    private void buildSinglePlayerPage(LinearLayout page) {
        // Launcher redesign F7 (LAUNCHER-REDESIGN 5.1): three panels -- the map list,
        // the selected map's preview, and the players / AI / mode / house rules. Every
        // control writes the same prefs as before; only the widgets changed (segments
        // and chips instead of dropdowns, radio buttons and checkboxes).
        final int wDp = getResources().getConfiguration().screenWidthDp;
        final boolean three = wDp >= 900, two = wDp >= 600;
        LinearLayout cols = new LinearLayout(this);
        cols.setOrientation(two ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        cols.setBaselineAligned(false);

        // --- maps ---
        LinearLayout mapsCol = spPanel();
        mapCountLabel = kit.label(S.t("Žemėlapiai"));
        mapCountLabel.setPadding(dp(4), 0, 0, dp(6));
        mapsCol.addView(mapCountLabel);
        mapList = new LinearLayout(this);
        mapList.setOrientation(LinearLayout.VERTICAL);
        ScrollView msv = new ScrollView(this);
        msv.setNestedScrollingEnabled(true);
        msv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        msv.addView(mapList);
        mapsCol.addView(msv, two ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(260)));

        // --- preview (three-column layout only; elsewhere a long-press opens it as a dialog) ---
        LinearLayout prevCol = null;
        if (three) {
            prevCol = spPanel();
            spPane = new PreviewPane();
            LinearLayout pin = spPane.build(S.t("Pasirink žemėlapį kairėje."));
            ScrollView psv0 = new ScrollView(this);
            psv0.setNestedScrollingEnabled(true);
            psv0.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
            psv0.addView(pin);
            prevCol.addView(psv0, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        }

        // --- players, AI, mode, house rules ---
        LinearLayout playersCol = spPanel();
        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        TextView pl = kit.label(S.t("Žaidėjai"));
        pl.setPadding(0, 0, 0, dp(4));
        right.addView(pl);
        playerRacesBox = new LinearLayout(this);
        playerRacesBox.setOrientation(LinearLayout.VERTICAL);
        right.addView(playerRacesBox);

        // player count 2..8 + two teams
        LinearLayout cntRow = new LinearLayout(this);
        cntRow.setOrientation(LinearLayout.HORIZONTAL);
        cntRow.setGravity(Gravity.CENTER_VERTICAL);
        cntRow.setPadding(0, dp(10), 0, 0);
        TextView cntLab = kit.label(S.t("Žaidėjų"));
        cntLab.setPadding(0, 0, dp(10), 0);
        cntRow.addView(cntLab);
        cntRow.addView(kit.segmented(new String[]{"2", "3", "4", "5", "6", "7", "8"}, clampCount(prefs().getInt("playerCount", 2)) - 2, idx -> {
            prefs().edit().putInt("playerCount", idx + 2).apply();
            rebuildPlayerRaces();
            if (mapList != null) populateMapList(mapList);
        }), new LinearLayout.LayoutParams(0, dp(44), 1f));
        TextView teams = kit.chip(S.t("Dvi komandos"), prefs().getInt("teamMode", 0) == 1, on -> {
            prefs().edit().putInt("teamMode", on ? 1 : 0).apply();
            rebuildPlayerRaces();
        });
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44));
        tlp.leftMargin = dp(10);
        cntRow.addView(teams, tlp);
        right.addView(cntRow);

        // opponent: the built-in AI's four levels + the original AI as segments; other
        // bots, "no AI" and bot import behind "More"
        right.addView(kit.label(S.t("AI lygis")));
        oppBox = new LinearLayout(this);
        oppBox.setOrientation(LinearLayout.HORIZONTAL);
        oppBox.setGravity(Gravity.CENTER_VERTICAL);
        right.addView(oppBox, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48)));
        if (!prefs().contains("vsAi")) prefs().edit().putBoolean("vsAi", true).apply();
        rebuildOpponentRow();

        // game mode
        right.addView(kit.label(S.t("Režimas")));
        right.addView(kit.segmented(GAME_MODE_LABELS, indexOfGameMode(prefs().getString("gameMode", "melee")),
                idx -> prefs().edit().putString("gameMode", GAME_MODE_VALUES[idx]).apply()),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48)));

        // SCBW2 house rules: negotiated game rules (single player defaults them on for
        // touch comfort); the bitmask ships to the sim via the "qol" extra (qolBits()).
        right.addView(kit.label(S.t("SCBW2 taisyklės")));
        Kit.Flow chips = new Kit.Flow(this, dp(8));
        addQolChip(chips, "Group rally", S.t("Group rally — visiems pažymėtiems pastatams"), "qol_grouprally", true);
        addQolChip(chips, "MBS", S.t("MBS — gamyba iš visų pažymėtų pastatų"), "qol_mbs", true);
        addQolChip(chips, "Smart-cast", S.t("Smart-cast — burtą meta tik geriausios energijos casteris"), "qol_smartcast", true);
        addQolChip(chips, ">12 selection", S.t("Iki 72 vienetų pažymėjime"), "qol_unlimitedsel", true);
        addQolChip(chips, "Auto-worker", S.t("Auto-worker — automatiškai gamina darbininkus (tik SP)"), "qol_autoworker", false);
        addQolChip(chips, S.t("Kelių pastatų žymėjimas"), S.t("Kelių pastatų žymėjimas — pastatai vienoje selekcijoje"), "qol_multiselect", false);
        right.addView(chips);

        ScrollView psv = new ScrollView(this);
        psv.setNestedScrollingEnabled(true);
        psv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        psv.addView(right);
        playersCol.addView(psv, two ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        rebuildPlayerRaces();

        if (two) {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
            l1.rightMargin = dp(10);
            cols.addView(mapsCol, l1);
            if (prevCol != null) {
                LinearLayout.LayoutParams l2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
                l2.rightMargin = dp(10);
                cols.addView(prevCol, l2);
            }
            cols.addView(playersCol, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.45f));
            page.addView(cols, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(480)));
            fitToPanel(cols);
        } else {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            l1.bottomMargin = dp(10);
            cols.addView(mapsCol, l1);
            cols.addView(playersCol, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            page.addView(cols);
        }
    }

    // A sub-panel inside the page panel (the columns of a screen).
    private LinearLayout spPanel() {
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setBackground(kit.box(0x8005080A, Kit.LINE));
        p.setPadding(dp(10), dp(10), dp(10), dp(10));
        return p;
    }

    // Columns that own their scrolling fill the page panel exactly (the panel is a
    // ScrollView, so "match parent" has nothing to match): follow its height.
    private void fitToPanel(final View v) { fitToPanel(v, 0); }
    // `less`: height taken by rows above the columns on the same page. One layout
    // listener serves every fitted view; views that left the tree are dropped (the
    // campaign screens are rebuilt on every visit).
    private final List<Object[]> fitTargets = new ArrayList<>();
    private boolean fitListening;
    private void fitToPanel(final View v, final int less) {
        if (pageScroll == null) return;
        fitTargets.add(new Object[]{v, less});
        if (!fitListening) {
            fitListening = true;
            pageScroll.addOnLayoutChangeListener((x, l, t, r, b, ol, ot, or, ob) -> { if (b - t != ob - ot) pageScroll.post(this::fitAll); });
        }
        pageScroll.post(this::fitAll);
    }
    private void fitAll() {
        if (pageScroll == null) return;
        int inner = pageScroll.getHeight() - pageScroll.getPaddingTop() - pageScroll.getPaddingBottom();
        for (java.util.Iterator<Object[]> it = fitTargets.iterator(); it.hasNext(); ) {
            Object[] e = it.next();
            View v = (View) e[0];
            if (v.getParent() == null) { it.remove(); continue; }
            int h = inner - (Integer) e[1];
            ViewGroup.LayoutParams lp = v.getLayoutParams();
            if (h > dp(200) && lp != null && lp.height != h) { lp.height = h; v.setLayoutParams(lp); }
        }
    }

    private void addQolChip(ViewGroup parent, String shortLabel, final String full, final String key, boolean def) {
        TextView c = kit.chip(shortLabel, prefs().getBoolean(key, def), on -> prefs().edit().putBoolean(key, on).apply());
        c.setOnLongClickListener(v -> { Toast.makeText(this, full, Toast.LENGTH_LONG).show(); return true; });
        parent.addView(c);
    }

    // Opponent picker. The pick is identified by STRUCTURE (kind / bot file / level),
    // never by a translated label (a label match once left useBot=false in every
    // non-Lithuanian locale).
    private void rebuildOpponentRow() {
        if (oppBox == null) return;
        oppBox.removeAllViews();
        java.io.File botsDir = new java.io.File(getFilesDir(), "bots");
        java.io.File[] botLibs = botsDir.isDirectory() ? botsDir.listFiles((d, n) -> n.endsWith(".so")) : null;
        final String UA = "libunifiedai.so";
        boolean haveUA = false;
        if (botLibs != null) for (java.io.File bl : botLibs) if (bl.getName().equals(UA)) haveUA = true;
        if (haveUA && !prefs().contains("useBot")) {
            // First run: the built-in AI at medium is the default opponent.
            prefs().edit().putBoolean("useBot", true).putBoolean("vsAi", false).putString("selectedBot", UA).putInt("botLevel", 2).apply();
        }
        final boolean useBot = prefs().getBoolean("useBot", false);
        final String selBot = selectedBotName(botLibs);
        final boolean vsAi = prefs().getBoolean("vsAi", true);

        // segments: [four levels of the built-in AI] + the original aiscript AI
        final String[] LEVEL_LT = {"Labai lengvas", "Lengvas", "Vidutinis", "Sunkus"};
        final ArrayList<String> segLabels = new ArrayList<>();
        final ArrayList<Integer> segLevel = new ArrayList<>();   // 0..3, or -1 = original AI
        if (haveUA) for (int lv = 0; lv < 4; ++lv) { segLabels.add(S.t(LEVEL_LT[lv])); segLevel.add(lv); }
        segLabels.add(S.t("Originalus"));
        segLevel.add(-1);
        int segSel = -1;
        if (useBot && UA.equals(selBot)) segSel = Math.max(0, Math.min(3, prefs().getInt("botLevel", 2)));
        else if (!useBot && vsAi) segSel = segLabels.size() - 1;
        oppBox.addView(kit.segmented(segLabels.toArray(new String[0]), segSel, idx -> {
            int lv = segLevel.get(idx);
            SharedPreferences.Editor e = prefs().edit();
            if (lv >= 0) e.putBoolean("useBot", true).putBoolean("vsAi", false).putString("selectedBot", UA).putInt("botLevel", lv);
            else e.putBoolean("useBot", false).putBoolean("vsAi", true);
            e.apply();
            rebuildOpponentRow();
            rebuildPlayerRaces();   // AI portraits follow the opponent kind / difficulty
        }), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));

        // "More": other installed bots (dev toggle), no AI, import a bot
        final ArrayList<String> more = new ArrayList<>();
        final ArrayList<String> moreBot = new ArrayList<>();     // .so name, "" = no AI, null = import
        if (botLibs != null && prefs().getBoolean("showLegacyBots", false)) {
            java.util.Arrays.sort(botLibs, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (java.io.File bl : botLibs) {
                if (bl.getName().equals(UA)) continue;
                more.add(S.t("Botas: ") + botDisplay(bl));
                moreBot.add(bl.getName());
            }
        }
        more.add(S.t("Be AI"));
        moreBot.add("");
        more.add(S.t("Importuoti botą…"));
        moreBot.add(null);
        String moreLabel = S.t("Daugiau");
        boolean moreOn = false;
        if (segSel < 0) {
            moreOn = true;
            moreLabel = S.t("Be AI");
            if (useBot && selBot != null) for (int k = 0; k != moreBot.size(); ++k)
                if (selBot.equals(moreBot.get(k))) moreLabel = more.get(k);
        }
        TextView moreBtn = kit.smallButton(moreLabel + "  \u25BE", () -> new AlertDialog.Builder(this)
                .setItems(more.toArray(new String[0]), (d, which) -> {
                    String f = moreBot.get(which);
                    if (f == null) { pickFiles(REQ_PICK_BOT); return; }
                    SharedPreferences.Editor e = prefs().edit();
                    if (f.isEmpty()) e.putBoolean("useBot", false).putBoolean("vsAi", false);
                    else e.putBoolean("useBot", true).putBoolean("vsAi", false).putString("selectedBot", f);
                    e.apply();
                    rebuildOpponentRow();
                    rebuildPlayerRaces();
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show());
        if (moreOn) { moreBtn.setBackground(kit.box(Kit.SEL_BG, Kit.LABEL)); moreBtn.setTextColor(Color.WHITE); }
        moreBtn.setMaxLines(1);
        moreBtn.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT);
        mlp.leftMargin = dp(8);
        oppBox.addView(moreBtn, mlp);
    }

    private int clampCount(int n) { return n < 2 ? 2 : (n > 8 ? 8 : n); }
    private int indexOfGameMode(String v) {
        for (int i = 0; i < GAME_MODE_VALUES.length; i++) if (GAME_MODE_VALUES[i].equals(v)) return i;
        return 0;
    }

    // A tap-to-open "dropdown": a button showing the current label; tapping lists
    // the options in an AlertDialog. Keeps the launcher's dialog-driven style.
    private interface IntSink { void accept(int i); }
    private Button dropdown(String[] labels, int selected, IntSink onPick) {
        Button b = new Button(this);
        b.setAllCaps(false);
        int cur = selected >= 0 && selected < labels.length ? selected : 0;
        b.setText(labels[cur] + "  ▾");
        b.setOnClickListener(v -> new AlertDialog.Builder(this)
                .setItems(labels, (d, which) -> {
                    b.setText(labels[which] + "  ▾");
                    onPick.accept(which);
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show());
        return b;
    }

    // A colour dropdown (name on a swatch-tinted button + a picker dialog). takenMask
    // bit N set = colour N is used by someone else (shown "(užimta)" and non-pickable);
    // -1 = nothing taken. `editable` false renders it read-only (a joiner viewing
    // another slot). onPick fires with the chosen index after a change.
    private View colorDropdown(int selected, int takenMask, boolean editable, IntSink onPick) {
        final int[] cur = { (selected >= 0 && selected < PLAYER_COLORS.length) ? selected : 0 };
        // The control itself is a small colour SQUARE showing the current pick.
        View btn = new View(this);
        int sz = dp(34);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(sz, sz);
        lp.setMargins(dp(8), dp(2), dp(2), dp(2));
        btn.setLayoutParams(lp);
        Runnable skin = () -> {
            android.graphics.drawable.GradientDrawable g = new android.graphics.drawable.GradientDrawable();
            g.setColor(PLAYER_COLORS[cur[0]]);
            g.setCornerRadius(dp(5));
            g.setStroke(dp(2), editable ? Color.rgb(210, 210, 220) : Color.rgb(90, 90, 100));
            btn.setBackground(g);
        };
        skin.run();
        if (editable) btn.setOnClickListener(v -> {
            // Tapping opens a palette grid (the old swatch row, now in a popup): tap a
            // swatch to pick it; colours held by others are dimmed + non-selectable.
            LinearLayout grid = new LinearLayout(this);
            grid.setOrientation(LinearLayout.VERTICAL);
            grid.setPadding(dp(18), dp(16), dp(18), dp(8));
            grid.setGravity(Gravity.CENTER_HORIZONTAL);
            final AlertDialog[] dlg = new AlertDialog[1];
            final int perRow = 5;
            LinearLayout[] rowRef = { null };
            int shown = 0;
            for (int i = 0; i < PLAYER_COLORS.length; i++) {
                if (i == PLAYER_COLOR_DUPLICATE) continue;
                if (shown++ % perRow == 0) { rowRef[0] = new LinearLayout(this); rowRef[0].setOrientation(LinearLayout.HORIZONTAL); grid.addView(rowRef[0]); }
                final int idx = i;
                boolean taken = takenMask >= 0 && idx != cur[0] && ((takenMask >> idx) & 1) != 0;
                View sw = new View(this);
                int ssz = dp(48);
                LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ssz, ssz);
                slp.setMargins(dp(6), dp(6), dp(6), dp(6));
                sw.setLayoutParams(slp);
                android.graphics.drawable.GradientDrawable sg = new android.graphics.drawable.GradientDrawable();
                sg.setColor(PLAYER_COLORS[idx]);
                sg.setCornerRadius(dp(7));
                sg.setStroke(dp(idx == cur[0] ? 4 : 1), idx == cur[0] ? Color.WHITE : Color.rgb(70, 70, 80));
                sw.setBackground(sg);
                sw.setAlpha(taken ? 0.30f : 1f);
                if (!taken) sw.setOnClickListener(x -> {
                    cur[0] = idx; skin.run(); onPick.accept(idx);
                    if (dlg[0] != null) dlg[0].dismiss();
                });
                rowRef[0].addView(sw);
            }
            dlg[0] = new AlertDialog.Builder(this).setTitle(S.t("Spalva")).setView(grid)
                    .setNegativeButton(S.t("Atšaukti"), null).create();
            dlg[0].show();
        });
        return btn;
    }
    private int spSlotColor(int slot) { return prefs().getInt("spColor." + slot, slot % PLAYER_COLORS.length); }

    // Row 0 is always the UserID profile; extra rows are AI opponents named
    // "<name>BOT". "Two teams" splits the rows into the two native-allied halves.
    private static final String[] BOT_NAMES = {
        "John","Karen","Mike","Sarah","Alex","Nina","Raj","Ivan","Lena","Omar","Zoe","Paul","Rita","Sven","Tara"};
    private void rebuildPlayerRaces() {
        if (playerRacesBox == null) return;
        playerRacesBox.removeAllViews();
        int count = clampCount(prefs().getInt("playerCount", 2));
        boolean twoTeams = prefs().getInt("teamMode", 0) == 1;
        int half = (count + 1) / 2;
        for (int slot = 0; slot < count; slot++) {
            if (twoTeams && slot == 0) playerRacesBox.addView(teamDivider("Komanda A"));
            if (twoTeams && slot == half) playerRacesBox.addView(teamDivider("Komanda B"));
            playerRacesBox.addView(playerRaceRow(slot));
        }
        updateActionSummary();
    }
    private TextView teamDivider(String s) {
        TextView t = kit.text(S.t(s).toUpperCase(Locale.ROOT), Kit.ACCENT, 11);
        t.setLetterSpacing(0.1f);
        t.setPadding(0, dp(8), 0, dp(2));
        return t;
    }
    private LinearLayout playerRaceRow(int slot) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(6), 0, dp(6));
        // Identity portrait: mine = the profile's; an AI slot = a unit by difficulty and
        // race; a Random AI = the "no signal" tile (the loading screen hides that race
        // too, so the portrait must not tell).
        final PortraitView pv = new PortraitView(this);
        pv.setScale(rowPortraitScale());
        pv.setBorderColor(PLAYER_COLORS[spSlotColor(slot) % PLAYER_COLORS.length]);
        File gameDir = getExternalFilesDir(null);
        pv.setPortrait(gameDir, slot == 0 ? PortraitStore.activeId(prefs(), activeProfile(), gameDir)
                : PortraitStore.aiPortrait(spAiLevel(), spSlotRace(slot)));
        row.addView(pv);
        LinearLayout who = new LinearLayout(this);
        who.setOrientation(LinearLayout.VERTICAL);
        who.setPadding(dp(12), 0, dp(8), 0);
        TextView name = kit.text(slot == 0 ? activeProfile() : botName(slot) + "BOT", Color.WHITE, 15);
        name.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        who.addView(name);
        int raceIdx = indexOfRace(spSlotRace(slot));
        // who plays the slot; the race is on the button right here, the level in the segments below
        who.addView(kit.text(slot == 0 ? S.t("tu") : opponentShortLabel(), Kit.TEXT2, 11));
        row.addView(who, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        // per-player colour (render-only, via --colors), then the race picker
        row.addView(colorDropdown(spSlotColor(slot), -1, true, idx -> {
            prefs().edit().putInt("spColor." + slot, idx).apply();
            pv.setBorderColor(PLAYER_COLORS[idx % PLAYER_COLORS.length]);
        }));
        final String[] races = raceNames();
        TextView raceBtn = kit.smallButton(races[raceIdx] + "  \u25BE", () -> new AlertDialog.Builder(this)
                .setItems(races, (d, which) -> {
                    prefs().edit().putInt("spRace." + slot, RACE_VALUES[which]).apply();
                    rebuildPlayerRaces();   // the row's caption and the AI portrait follow the race
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show());
        raceBtn.setMinWidth(dp(96));
        raceBtn.setMinimumWidth(dp(96));
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40));
        rlp.leftMargin = dp(8);
        row.addView(raceBtn, rlp);
        col.addView(row);
        View line = new View(this);
        line.setBackgroundColor(Kit.LINE);
        col.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
        return col;
    }
    private String opponentShortLabel() {
        boolean bot = prefs().getBoolean("useBot", false);
        if (spAiLevel() >= 0 || (!bot && prefs().getBoolean("vsAi", true))) return "AI";
        return bot ? S.t("Botas") : S.t("Be AI");
    }
    // "AI · Medium" / "Original AI" / "Bot" / "No AI" -- what plays the opponent slots.
    private String opponentKindLabel() {
        int lv = spAiLevel();
        final String[] LEVEL_LT = {"Labai lengvas", "Lengvas", "Vidutinis", "Sunkus"};
        if (lv >= 0) return "AI · " + S.t(LEVEL_LT[Math.min(3, lv)]);
        if (prefs().getBoolean("useBot", false)) return S.t("Botas");
        if (prefs().getBoolean("vsAi", true)) return S.t("Originalus AI");
        return S.t("Be AI");
    }
    // Difficulty of the built-in AI for the portrait rule; -1 = the original aiscript
    // AI, another bot, or no AI at all (those show the race advisor).
    private int spAiLevel() {
        if (prefs().getBoolean("useBot", false) && "libunifiedai.so".equals(prefs().getString("selectedBot", "")))
            return prefs().getInt("botLevel", 2);
        return -1;
    }
    // Portraits in list rows: the whole multiple of the 56 px clip nearest to 44 dp.
    private int rowPortraitScale() { return Math.max(1, Math.round(dp(44) / 56f)); }
    private int indexOfRace(int raceValue) {
        for (int i = 0; i < RACE_VALUES.length; i++) if (RACE_VALUES[i] == raceValue) return i;
        return RACE_VALUES.length - 1; // random
    }
    private int spSlotRace(int slot) {
        int def = slot == 0 ? prefs().getInt("race", 5) : 5;
        return prefs().getInt("spRace." + slot, def);
    }
    private String botName(int slot) {
        String key = "spBotName." + slot;
        String n = prefs().getString(key, "");
        if (n.isEmpty()) { n = BOT_NAMES[(slot * 7 + 3) % BOT_NAMES.length]; prefs().edit().putString(key, n).apply(); }
        return n;
    }
    private String spRacesCsv() {
        int count = clampCount(prefs().getInt("playerCount", 2));
        StringBuilder sb = new StringBuilder();
        for (int s = 0; s < count; s++) { if (s > 0) sb.append(","); sb.append(spSlotRace(s)); }
        return sb.toString();
    }
    // #1: per-slot player colours (index 0 = local), applied to every slot via --colors.
    private String spColorsCsv() {
        int count = clampCount(prefs().getInt("playerCount", 2));
        StringBuilder sb = new StringBuilder();
        for (int s = 0; s < count; s++) { if (s > 0) sb.append(","); sb.append(spSlotColor(s)); }
        return sb.toString();
    }

    // v0.46 MULTIPLAYER (LAN) page (#6): a launcher-side live lobby. Create hosts
    // it, Join browses live WiFi hosts; slots/races/ready sync while everyone is
    // still in the launcher (LanLobby), and the game only launches once the host
    // starts. Map-aware: a joiner that lacks the host's map is blocked with an error.
    private LinearLayout mpSetup, mpLobbyBox, mpHostList, mpSlotsBox;
    private TextView mpLobbyTitle, mpLobbyStatus;
    private LanLobby mpLobby;
    private LanLobby.State mpState;
    private boolean mpIsHost;

    private ScrollView mpScroll(LinearLayout inner, int heightDp) {
        inner.setOrientation(LinearLayout.VERTICAL);
        ScrollView sv = new ScrollView(this);
        sv.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(heightDp)));
        sv.setNestedScrollingEnabled(true);
        sv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        sv.addView(inner);
        return sv;
    }

    private void buildMultiplayerPage(LinearLayout page) {
        // Launcher redesign F7 (LAUNCHER-REDESIGN 5.2-5.4). Same LanLobby flow as
        // before; the primary actions (Create lobby / Join / Ready / Start / Leave)
        // live in the action bar.
        final int wDp = getResources().getConfiguration().screenWidthDp;
        final boolean three = wDp >= 900, two = wDp >= 600;
        mpSetup = new LinearLayout(this);
        mpSetup.setOrientation(LinearLayout.VERTICAL);
        page.addView(mpSetup);
        mpLobbyBox = new LinearLayout(this);
        mpLobbyBox.setOrientation(LinearLayout.VERTICAL);
        mpLobbyBox.setVisibility(View.GONE);
        page.addView(mpLobbyBox);
        buildLobbyView(two);

        // Create | Join, and (Join) "join by IP"
        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(0, 0, 0, dp(8));
        top.addView(kit.segmented(new String[]{S.t("Sukurti"), S.t("Prisijungti")}, mpSub, idx -> { mpSub = idx; applyMpSub(); }),
                new LinearLayout.LayoutParams(dp(two ? 320 : 220), dp(44)));
        top.addView(new android.widget.Space(this), new LinearLayout.LayoutParams(0, 1, 1f));
        mpJoinTools = new LinearLayout(this);
        mpJoinTools.setOrientation(LinearLayout.HORIZONTAL);
        mpJoinTools.setGravity(Gravity.CENTER_VERTICAL);
        joinIpField = new EditText(this);
        joinIpField.setHint(S.t("Prisijungti pagal IP…"));
        joinIpField.setSingleLine(true);
        joinIpField.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_URI);
        joinIpField.setTextColor(Kit.TEXT);
        joinIpField.setHintTextColor(Kit.TEXT2);
        joinIpField.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        joinIpField.setBackground(kit.box(0xFF05080A, Kit.STEEL));
        joinIpField.setPadding(dp(10), 0, dp(10), 0);
        mpJoinTools.addView(joinIpField, new LinearLayout.LayoutParams(dp(200), dp(44)));
        LinearLayout.LayoutParams jb = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44));
        jb.leftMargin = dp(8);
        mpJoinTools.addView(kit.smallButton(S.t("Jungtis"), this::mpJoinByIp), jb);
        top.addView(mpJoinTools);
        mpSetup.addView(top);

        // ---- Create: map | preview | host settings ----
        mpCreateBox = new LinearLayout(this);
        mpCreateBox.setOrientation(two ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        mpCreateBox.setBaselineAligned(false);
        LinearLayout mapsCol = spPanel();
        mpMapCountLabel = kit.label(S.t("Žemėlapiai"));
        mpMapCountLabel.setPadding(dp(4), 0, 0, dp(6));
        mapsCol.addView(mpMapCountLabel);
        mapListMp = new LinearLayout(this);
        mapListMp.setOrientation(LinearLayout.VERTICAL);
        ScrollView mapSv = new ScrollView(this);
        mapSv.setNestedScrollingEnabled(true);
        mapSv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        mapSv.addView(mapListMp);
        mapsCol.addView(mapSv, two ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(260)));
        LinearLayout prevCol = null;
        if (three) {
            prevCol = spPanel();
            mpPane = new PreviewPane();
            mpPane.addTo(prevCol, S.t("Pasirink žemėlapį kairėje."));
        }
        LinearLayout hostCol = spPanel();
        LinearLayout hc = new LinearLayout(this);
        hc.setOrientation(LinearLayout.VERTICAL);
        TextView rl = kit.label(S.t("Tavo rasė"));
        rl.setPadding(0, 0, 0, dp(6));
        hc.addView(rl);
        mpRaceCreate = new LinearLayout(this);
        hc.addView(mpRaceCreate, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44)));
        hc.addView(kit.label(S.t("Žaidėjų skaičius")));
        hc.addView(kit.segmented(new String[]{"2", "3", "4", "5", "6", "7", "8"}, clampCount(prefs().getInt("playerCount", 2)) - 2, idx -> {
            prefs().edit().putInt("playerCount", idx + 2).apply();
            if (mapListMp != null) populateMapList(mapListMp);
            updateActionSummary();
        }), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44)));
        // The HOST picks the SCBW2 house rules; native negotiates them to every peer at
        // start, so the whole LAN session runs the host's rules. (Auto-worker is
        // single-player only -- its injector is net-gated off -- so it is not offered.)
        hc.addView(kit.label(S.t("SCBW2 taisyklės")));
        Kit.Flow chips = new Kit.Flow(this, dp(8));
        addQolChip(chips, "Group rally", S.t("Group rally — visiems pažymėtiems pastatams"), "qol_grouprally", true);
        addQolChip(chips, "MBS", S.t("MBS — gamyba iš visų pažymėtų pastatų"), "qol_mbs", true);
        addQolChip(chips, "Smart-cast", S.t("Smart-cast — burtą meta tik geriausios energijos casteris"), "qol_smartcast", true);
        addQolChip(chips, ">12 selection", S.t("Iki 72 vienetų pažymėjime"), "qol_unlimitedsel", true);
        addQolChip(chips, S.t("Kelių pastatų žymėjimas"), S.t("Kelių pastatų žymėjimas — pastatai vienoje selekcijoje"), "qol_multiselect", false);
        hc.addView(chips);
        ScrollView hsv = new ScrollView(this);
        hsv.setNestedScrollingEnabled(true);
        hsv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        hsv.addView(hc);
        hostCol.addView(hsv, two ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        if (two) {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
            l1.rightMargin = dp(10);
            mpCreateBox.addView(mapsCol, l1);
            if (prevCol != null) {
                LinearLayout.LayoutParams l2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
                l2.rightMargin = dp(10);
                mpCreateBox.addView(prevCol, l2);
            }
            mpCreateBox.addView(hostCol, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.45f));
            mpSetup.addView(mpCreateBox, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(430)));
            fitToPanel(mpCreateBox, dp(52));
        } else {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            l1.bottomMargin = dp(10);
            mpCreateBox.addView(mapsCol, l1);
            mpCreateBox.addView(hostCol, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            mpSetup.addView(mpCreateBox);
        }

        // ---- Join: live hosts | my race + the picked host's map ----
        mpJoinBox = new LinearLayout(this);
        mpJoinBox.setOrientation(two ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        mpJoinBox.setBaselineAligned(false);
        LinearLayout hostsCol = spPanel();
        mpHostsLabel = kit.label(S.t("Hostai"));
        mpHostsLabel.setPadding(dp(4), 0, 0, dp(6));
        hostsCol.addView(mpHostsLabel);
        mpHostList = new LinearLayout(this);
        mpHostList.setOrientation(LinearLayout.VERTICAL);
        ScrollView hostSv = new ScrollView(this);
        hostSv.setNestedScrollingEnabled(true);
        hostSv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        hostSv.addView(mpHostList);
        hostsCol.addView(hostSv, two ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(260)));
        LinearLayout meCol = spPanel();
        TextView rl2 = kit.label(S.t("Tavo rasė"));
        rl2.setPadding(0, 0, 0, dp(6));
        meCol.addView(rl2);
        mpRaceJoin = new LinearLayout(this);
        LinearLayout.LayoutParams rjl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44));
        rjl.bottomMargin = dp(10);
        meCol.addView(mpRaceJoin, rjl);
        if (two) {
            joinPane = new PreviewPane();
            joinPane.addTo(meCol, "");
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.7f);
            l1.rightMargin = dp(10);
            mpJoinBox.addView(hostsCol, l1);
            mpJoinBox.addView(meCol, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
            mpSetup.addView(mpJoinBox, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(430)));
            fitToPanel(mpJoinBox, dp(52));
        } else {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            l1.bottomMargin = dp(10);
            mpJoinBox.addView(meCol, l1);
            mpJoinBox.addView(hostsCol, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            mpSetup.addView(mpJoinBox);
        }
        applyMpSub();
    }

    // The "your race" segments exist on both sub-screens and share the "race" pref:
    // rebuilt on every switch so they never disagree.
    private void fillRaceSegments(LinearLayout holder) {
        if (holder == null) return;
        holder.removeAllViews();
        holder.addView(kit.segmented(raceNames(), indexOfRace(prefs().getInt("race", 5)),
                idx -> prefs().edit().putInt("race", RACE_VALUES[idx]).apply()),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private void applyMpSub() {
        boolean host = mpSub == 0;
        mpCreateBox.setVisibility(host ? View.VISIBLE : View.GONE);
        mpJoinBox.setVisibility(host ? View.GONE : View.VISIBLE);
        mpJoinTools.setVisibility(host ? View.GONE : View.VISIBLE);
        fillRaceSegments(host ? mpRaceCreate : mpRaceJoin);
        if (!host) mpScanHosts();
        updateStartButtonVisibility();
    }

    // Join by address: ask that one device what it hosts, then join it like a scanned host.
    private void mpJoinByIp() {
        final String ip = joinIpField.getText().toString().trim();
        if (ip.isEmpty()) { Toast.makeText(this, S.t("Įveskite host IP."), Toast.LENGTH_LONG).show(); return; }
        Toast.makeText(this, S.t("Ieškoma…"), Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            final List<java.net.InetAddress> one = new ArrayList<>();
            try { one.add(java.net.InetAddress.getByName(ip)); } catch (Exception ignored) {}
            final List<LanLobby.HostInfo> hosts = one.isEmpty() ? new ArrayList<LanLobby.HostInfo>() : LanLobby.scan(one, 1500);
            ui.post(() -> {
                if (hosts.isEmpty()) Toast.makeText(this, S.t("Hostas neatsako: ") + ip, Toast.LENGTH_LONG).show();
                else mpJoinHost(hosts.get(0));
            });
        }, "mp-ip").start();
    }

    // The live lobby view (players | map), hidden until in a lobby.
    private void buildLobbyView(boolean two) {
        LinearLayout cols = new LinearLayout(this);
        cols.setOrientation(two ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        cols.setBaselineAligned(false);
        LinearLayout playersCol = spPanel();
        mpLobbyTitle = kit.label(S.t("Žaidėjai"));
        mpLobbyTitle.setPadding(dp(4), 0, 0, dp(6));
        playersCol.addView(mpLobbyTitle);
        mpSlotsBox = new LinearLayout(this);
        mpSlotsBox.setOrientation(LinearLayout.VERTICAL);
        ScrollView ssv = new ScrollView(this);
        ssv.setNestedScrollingEnabled(true);
        ssv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
        ssv.addView(mpSlotsBox);
        playersCol.addView(ssv, two ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout mapCol = spPanel();
        mpLobbyStatus = kit.text("", Kit.TEXT2, 12);
        if (two) {
            lobbyPane = new PreviewPane();
            lobbyPane.addTo(mapCol, "");
        }
        mpLobbyStatus.setPadding(0, dp(6), 0, 0);
        mapCol.addView(mpLobbyStatus);
        if (two) {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.7f);
            l1.rightMargin = dp(10);
            cols.addView(playersCol, l1);
            cols.addView(mapCol, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
            mpLobbyBox.addView(cols, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(480)));
            fitToPanel(cols, 0);
        } else {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            l1.bottomMargin = dp(10);
            cols.addView(playersCol, l1);
            cols.addView(mapCol, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            mpLobbyBox.addView(cols);
        }
    }

    private boolean mpLocalReady;

    private void mpToggleReady() {
        mpLocalReady = !mpLocalReady;
        updateStartButtonVisibility();
        if (mpLobby == null) return;
        if (mpIsHost) mpLobby.setHostReady(mpLocalReady);
        else mpLobby.setJoinReady(mpLocalReady);
    }

    private void mpStartHosting() {
        if (!mpqMissing().isEmpty()) { Toast.makeText(this, "Pirma importuokite MPQ.", Toast.LENGTH_LONG).show(); return; }
        if (selectedMap == null && selectedMpqMap == null) {
            Toast.makeText(this, S.t("Pasirinkite žemėlapį."), Toast.LENGTH_LONG).show();
            return;
        }
        String mapId = selectedMpqMap != null ? selectedMpqMap : selectedMap.getName();
        int players = clampCount(prefs().getInt("playerCount", 2));
        int race = prefs().getInt("race", 5);
        int speed = prefs().getInt("gameSpeed", 100);
        mpIsHost = true;
        mpLocalReady = false;
        mpLobby = new LanLobby(mpLobbyListener, versionName(), enabledModsCsv());
        mpLobby.setLocalPortrait(PortraitStore.activeId(prefs(), activeProfile(), getExternalFilesDir(null)));
        mpLobby.startHost(activeProfile(), race, mapId, players, speed);
        showLobbyView();
    }

    // Live host scan for the Join sub-tab; each result is map-aware.
    private void mpScanHosts() {
        if (mpHostList == null) return;
        mpHostList.removeAllViews();
        mpHostList.addView(emptyLabel(S.t("Ieškoma…")));
        final List<java.net.InetAddress> bcast = broadcastAddresses();
        new Thread(() -> {
            final List<LanLobby.HostInfo> hosts = LanLobby.scan(bcast, 1500);
            new Handler(Looper.getMainLooper()).post(() -> {
                mpHostList.removeAllViews();
                mpSelHost = null;
                mpSelHostCard = null;
                if (joinPane != null) joinPane.clear("");
                if (mpHostsLabel != null) mpHostsLabel.setText((S.t("Hostai") + "  ·  " + hosts.size()).toUpperCase(Locale.ROOT));
                updateStartButtonVisibility();
                if (hosts.isEmpty()) {
                    mpHostList.addView(emptyLabel(S.t("Hostų nerasta. Kitame įrenginyje paspausk „Sukurti lobby“.")));
                    return;
                }
                for (LanLobby.HostInfo h : hosts) mpHostList.addView(hostCard(h));
            });
        }, "mp-scan").start();
    }

    private View hostCard(final LanLobby.HostInfo h) {
        final boolean have = mpHaveMap(h.map);
        final LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(8), dp(8), dp(10), dp(8));
        card.setBackground(kit.box(0xFF070B0E, Kit.STEEL));
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        clp.bottomMargin = dp(8);
        card.setLayoutParams(clp);
        // the host's portrait (an older host sends none -> "no signal": its race is unknown here)
        PortraitView pv = new PortraitView(this);
        pv.setScale(rowPortraitScale());
        pv.setBorderColor(Kit.STEEL);
        pv.setPortrait(getExternalFilesDir(null), h.portrait);
        card.addView(pv);
        LinearLayout mid = new LinearLayout(this);
        mid.setOrientation(LinearLayout.VERTICAL);
        mid.setPadding(dp(12), 0, dp(8), 0);
        TextView t = kit.text(mapBasename(h.map) + "    " + h.used + " / " + h.total, have ? Color.WHITE : Kit.DANGER, 15);
        t.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        mid.addView(t);
        mid.addView(kit.text((h.name.isEmpty() ? h.ip : h.name) + "  ·  " + S.t("greitis: ") + gameSpeedName(String.valueOf(h.speed)), Kit.TEXT2, 12));
        card.addView(mid, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        // what stands in the way, if anything
        String myMods = enabledModsCsv();
        if (myMods == null || myMods.isEmpty()) myMods = "-";
        String tag = null;
        int tagColor = Kit.ACCENT;
        if (!have) { tag = S.t("neturi šio žemėlapio"); tagColor = Kit.DANGER; }
        else if (!h.ver.isEmpty() && !h.ver.equals(versionName())) tag = S.t("kita versija ") + h.ver;
        else if (!h.mods.isEmpty() && !h.mods.equals(myMods)) tag = S.t("modai skiriasi");
        if (tag != null) {
            TextView g = kit.text(tag, tagColor, 11);
            g.setBackground(kit.box(0x00000000, tagColor));
            g.setPadding(dp(6), dp(1), dp(6), dp(1));
            card.addView(g);
        }
        card.setOnClickListener(v -> {
            if (mpSelHostCard != null) mpSelHostCard.setBackground(kit.box(0xFF070B0E, Kit.STEEL));
            mpSelHost = h;
            mpSelHostCard = card;
            card.setBackground(kit.box(Kit.SEL_BG, Kit.LABEL));
            if (joinPane != null) { if (have) joinPane.showMapId(h.map); else joinPane.clear(mapBasename(h.map)); }
            updateStartButtonVisibility();
        });
        // long-press a host whose map we have -> the large minimap preview
        if (have) {
            card.setOnLongClickListener(v -> {
                if (h.map.indexOf('/') >= 0) {
                    showMinimapPreview(mapBasename(h.map), true, h.map);
                } else {
                    File f = resolveMapFile(h.map);
                    if (f != null) showMinimapPreview(h.map, false, f.getAbsolutePath());
                }
                return true;
            });
        }
        return card;
    }

    private void mpJoinHost(LanLobby.HostInfo h) {
        if (!mpHaveMap(h.map)) {
            Toast.makeText(this, "Tu neturi parsisiuntęs šio žemėlapio: " + mapBasename(h.map)
                    + ". Importuok jį arba pridėk MPQ.", Toast.LENGTH_LONG).show();
            return;
        }
        mpResolveMapSelection(h.map);
        int race = prefs().getInt("race", 5);
        mpIsHost = false;
        mpLocalReady = false;
        mpLobby = new LanLobby(mpLobbyListener, versionName(), enabledModsCsv());
        mpLobby.setLocalPortrait(PortraitStore.activeId(prefs(), activeProfile(), getExternalFilesDir(null)));
        mpLobby.startJoin(activeProfile(), race, h.ip);
        showLobbyView();
    }

    // Map identity helpers: a bundled map carries its MPQ path (has '/'), an
    // imported map just its filename. mapBasename() is the display name.
    private String mapBasename(String id) {
        if (id == null) return "?";
        int s = id.lastIndexOf('/');
        return s >= 0 ? id.substring(s + 1) : id;
    }
    private boolean mpHaveMap(String id) {
        if (id == null || id.isEmpty()) return false;
        if (id.indexOf('/') >= 0) {
            // Bundled: needs the CD MPQ archives that hold it.
            for (Episode grp : bundledMapGroups())
                for (String[] m : grp.missions) if (m[1].equals(id)) {
                    return new File(getExternalFilesDir(null), "StarCraft.mpq").exists()
                            || new File(getExternalFilesDir(null), "BroodWar.mpq").exists();
                }
            return false;
        }
        return resolveMapFile(id) != null;   // v0.83.033: also matches active map-pack mods
    }
    private void mpResolveMapSelection(String id) {
        if (id.indexOf('/') >= 0) { selectedMpqMap = id; selectedMap = null; }
        else {
            // v0.83.033: prefer a real file (files/maps OR an active mod's maps/);
            // fall back to the plain path so behaviour is unchanged when absent.
            File f = resolveMapFile(id);
            selectedMap = f != null ? f : new File(new File(getExternalFilesDir(null), "maps"), id);
            selectedMpqMap = null;
        }
        selectedReplay = null;
    }

    private void showLobbyView() {
        mpSetup.setVisibility(View.GONE);
        mpLobbyBox.setVisibility(View.VISIBLE);
        lobbyPreviewMap = "";
        updateStartButtonVisibility();
    }
    private void mpLeaveLobby() {
        if (mpLobby != null) { mpLobby.close(); mpLobby = null; }
        mpState = null;
        mpLobbyBox.setVisibility(View.GONE);
        mpSetup.setVisibility(View.VISIBLE);
        if (screenTitle != null && activeTab == TAB_MULTI) screenTitle.setText(S.t(SCREEN_TITLE[TAB_MULTI]).toUpperCase(Locale.ROOT));
        updateStartButtonVisibility();
    }

    private void renderLobby(LanLobby.State s) {
        mpState = s;
        mpLobbyTitle.setText((S.t("Žaidėjai") + "  ·  " + s.slots.size() + " / " + s.target).toUpperCase(Locale.ROOT));
        mpLobbyStatus.setText(mapBasename(s.map) + "  ·  " + S.t("greitis: ") + gameSpeedName(String.valueOf(s.speed)));
        if (lobbyPane != null && !s.map.equals(lobbyPreviewMap)) { lobbyPreviewMap = s.map; lobbyPane.showMapId(s.map); }
        if (screenTitle != null && activeTab == TAB_MULTI)
            screenTitle.setText("LAN LOBBY");
        mpSlotsBox.removeAllViews();
        for (int i = 0; i < s.slots.size(); i++) {
            LanLobby.Slot sl = s.slots.get(i);
            LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(4), dp(8), dp(4), dp(8));
            // The slot's portrait: what its launcher sent; an older build (-1) shows the
            // race advisor, an unknown race the "no signal" tile.
            int shownColor = (sl.color >= 0 && sl.color < PLAYER_COLORS.length) ? sl.color : (i % PLAYER_COLORS.length);
            PortraitView lpv = new PortraitView(this);
            lpv.setScale(rowPortraitScale());
            lpv.setBorderColor(PLAYER_COLORS[shownColor]);
            lpv.setPortrait(getExternalFilesDir(null), sl.portrait >= 0 ? sl.portrait : PortraitStore.aiPortrait(-1, sl.race));
            row.addView(lpv);
            LinearLayout who = new LinearLayout(this);
            who.setOrientation(LinearLayout.VERTICAL);
            who.setPadding(dp(12), 0, dp(8), 0);
            TextView name = kit.text(sl.userId + (sl.host ? "   HOST" : ""), Color.WHITE, 15);
            name.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
            if (sl.host) {
                android.text.SpannableString sp = new android.text.SpannableString(name.getText());
                int at = sp.length() - 4;
                sp.setSpan(new android.text.style.ForegroundColorSpan(Kit.ACCENT), at, sp.length(), 0);
                sp.setSpan(new android.text.style.RelativeSizeSpan(0.7f), at, sp.length(), 0);
                name.setText(sp);
            }
            who.addView(name);
            boolean isMe = (mpIsHost && i == 0) || (!mpIsHost && sl.userId.equals(activeProfile()) && !sl.host);
            // the race of the others; my own is on the race button of my row
            if (!isMe) who.addView(kit.text(sl.race >= 0 && sl.race <= 2 ? raceNames()[indexOfRace(sl.race)] : S.t("Atsitiktinė"), Kit.TEXT2, 11));
            row.addView(who, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            // Colour. Editable when it's my slot OR I'm the host (the host may recolour
            // anyone); a joiner can only change its own. Colours held by OTHER slots are
            // dimmed + non-pickable (host enforces via colorFree), so no two players share
            // a colour. My own pick also updates "playerColor" -> --player-color.
            final int slotIdx = i;
            int taken = 0;
            for (int k = 0; k < s.slots.size(); k++) {
                if (k == i) continue;
                int c = s.slots.get(k).color;
                if (c >= 0 && c < PLAYER_COLORS.length) taken |= (1 << c);
            }
            boolean canEdit = isMe || mpIsHost;
            if (isMe && sl.color >= 0) prefs().edit().putInt("playerColor", sl.color).apply();
            row.addView(colorDropdown(shownColor, taken, canEdit, col2 -> {
                if (mpLobby == null) return;
                if (isMe) {
                    prefs().edit().putInt("playerColor", col2).apply();
                    if (mpIsHost) mpLobby.setHostColor(col2); else mpLobby.setJoinColor(col2);
                } else if (mpIsHost) {
                    mpLobby.setSlotColor(slotIdx, col2); // host recolours another slot
                }
            }));
            // Race picker on my own slot.
            if (isMe) {
                final String[] races = raceNames();
                TextView raceBtn = kit.smallButton(races[indexOfRace(sl.race)] + "  \u25BE", () -> new AlertDialog.Builder(this)
                        .setItems(races, (d, which) -> {
                            int rv = RACE_VALUES[which];
                            prefs().edit().putInt("race", rv).apply();
                            if (mpLobby != null) { if (mpIsHost) mpLobby.setHostRace(rv); else mpLobby.setJoinRace(rv); }
                        })
                        .setNegativeButton(S.t("Atšaukti"), null)
                        .show());
                raceBtn.setMinWidth(dp(96));
                raceBtn.setMinimumWidth(dp(96));
                LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40));
                rlp.leftMargin = dp(8);
                row.addView(raceBtn, rlp);
            }
            TextView st = kit.text(sl.ready ? "\u2713 " + S.t("pasiruošęs") : S.t("laukia"), sl.ready ? Kit.LABEL : Kit.ACCENT, 11);
            st.setBackground(kit.box(0x00000000, sl.ready ? Kit.LABEL : Kit.ACCENT));
            st.setPadding(dp(8), dp(2), dp(8), dp(2));
            LinearLayout.LayoutParams stl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            stl.leftMargin = dp(10);
            row.addView(st, stl);
            col.addView(row);
            View line = new View(this);
            line.setBackgroundColor(Kit.LINE);
            col.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
            mpSlotsBox.addView(col);
        }
        updateStartButtonVisibility();
    }

    private void launchNetGame(boolean isHost, String hostIp, int race, int speed, int players) {
        Intent i = new Intent(this, OpenBWActivity.class);
        if (selectedMpqMap != null) i.putExtra(OpenBWActivity.EXTRA_MAP_MPQ, selectedMpqMap);
        else if (selectedMap != null) i.putExtra(OpenBWActivity.EXTRA_MAP, selectedMap.getAbsolutePath());
        else { Toast.makeText(this, S.t("Žemėlapis nerastas."), Toast.LENGTH_LONG).show(); return; }
        i.putExtra(OpenBWActivity.EXTRA_RACE, race);
        i.putExtra(OpenBWActivity.EXTRA_PLAYERS, players > 0 ? players : clampCount(prefs().getInt("playerCount", 2)));
        if (isHost) i.putExtra(OpenBWActivity.EXTRA_NET_HOST, true);
        else i.putExtra(OpenBWActivity.EXTRA_NET_JOIN, hostIp);
        // task 1: the HOST negotiates the SCBW2 house-rules to the whole LAN session
        // (native broadcasts qol_bits with the start-game message). A joiner sends no
        // qol -- it adopts the host's -- so only pass it when hosting.
        if (isHost) i.putExtra("qol", qolBits());
        // The launcher lobby already agreed on slots/races/ready, so the native
        // side should auto-start on connect instead of asking to ready up again.
        i.putExtra(OpenBWActivity.EXTRA_NET_FAST, true);
        // Run the game on a DIFFERENT port than the launcher lobby (6112): the
        // lobby socket may still be in TIME_WAIT when the game binds, which was
        // racily failing the host's bind and stranding both on the loading screen.
        i.putExtra(OpenBWActivity.EXTRA_NET_PORT, 6113);
        // Identity portraits of the others: only ids travel, as a name -> id list from
        // the launcher lobby (nothing is added to the lockstep stream). Render-only.
        if (mpState != null) {
            StringBuilder np = new StringBuilder();
            for (LanLobby.Slot sl : mpState.slots) {
                if (sl.portrait < 0 || sl.userId == null || sl.userId.isEmpty()) continue;
                if (np.length() > 0) np.append('|');
                np.append(sl.portrait).append('=').append(sl.userId.replace('|', '_'));
            }
            if (np.length() > 0) i.putExtra(OpenBWActivity.EXTRA_NAME_PORTRAITS, np.toString());
        }
        prefs().edit().putInt("lastLocalRace", race).apply();
        addCommonExtras(i);
        if (speed >= 25 && speed <= 800) i.putExtra(OpenBWActivity.EXTRA_GAME_SPEED, speed);
        if (selectedMpqMap != null && selectedMpqMap.contains("/scenario/"))
            i.putExtra(OpenBWActivity.EXTRA_GAME_MODE, "ums");
        startActivity(i);
    }

    private final LanLobby.Listener mpLobbyListener = new LanLobby.Listener() {
        @Override public void onState(LanLobby.State s) { renderLobby(s); }
        @Override public void onStart(boolean isHost, String hostIp, int myRace, int speed, int players) {
            MenuAudio.play(MenuAudio.COUNTDOWN);
            // Use the live prefs race (the lobby dropdown writes it) so a
            // last-second change is honoured; host speed/target from the snapshot.
            int race = prefs().getInt("race", 5);
            int sp = speed > 0 ? speed : (mpState != null ? mpState.speed : prefs().getInt("gameSpeed", 100));
            // Actual players present (joiner falls back to the live slot count, not
            // the host's target) so nobody waits for a peer that never joined.
            int pl = players > 0 ? players : (mpState != null ? mpState.slots.size() : 0);
            mpLobby = null;
            mpLobbyBox.setVisibility(View.GONE);
            mpSetup.setVisibility(View.VISIBLE);
            if (screenTitle != null && activeTab == TAB_MULTI) screenTitle.setText(S.t(SCREEN_TITLE[TAB_MULTI]).toUpperCase(Locale.ROOT));
            updateStartButtonVisibility();
            launchNetGame(isHost, hostIp, race, sp, pl);
        }
        @Override public void onError(String msg) {
            Toast.makeText(SetupActivity.this, S.t(msg), Toast.LENGTH_LONG).show();
            mpLeaveLobby();
        }
        @Override public void onClosed() { }
    };

    /**
     * UDP LAN discovery (client side). Broadcasts "OPENBW_DISCOVER" and lists
     * hosts that reply with "OPENBW\t&lt;port&gt;\t&lt;map&gt;". Picking one
     * fills the join IP field. The host answers from its lobby (native side).
     */

    /** IPv4 broadcast addresses of all up interfaces, plus the global one. */
    private List<java.net.InetAddress> broadcastAddresses() {
        List<java.net.InetAddress> out = new ArrayList<>();
        try {
            for (java.util.Enumeration<java.net.NetworkInterface> en =
                    java.net.NetworkInterface.getNetworkInterfaces(); en.hasMoreElements();) {
                java.net.NetworkInterface ni = en.nextElement();
                if (ni.isLoopback() || !ni.isUp()) continue;
                for (java.net.InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    java.net.InetAddress b = ia.getBroadcast();
                    if (b != null) out.add(b);
                }
            }
            out.add(java.net.InetAddress.getByName("255.255.255.255"));
        } catch (Exception ignored) {
        }
        return out;
    }

    // The action bar calls this for two screens only: Single player (Start game) and
    // Replays (Watch). LAN and the campaign launch through their own paths
    // (launchNetGame / launchCampaignMission).
    private void startGame() {
        if (!mpqMissing().isEmpty()) {
            Toast.makeText(this, S.t("Pirma importuokite MPQ failus."), Toast.LENGTH_LONG).show();
            return;
        }
        if (activeTab == TAB_REPLAY) {
            if (selectedReplay == null) return;
            Intent i = new Intent(this, OpenBWActivity.class);
            i.putExtra(OpenBWActivity.EXTRA_REPLAY, selectedReplay.getAbsolutePath());
            addCommonExtras(i);
            // Coming back (EXIT / Back) lands on the Replays list with this entry
            // still selected, also when the launcher was recreated meanwhile.
            prefs().edit().putString("returnToReplay", selectedReplay.getAbsolutePath()).apply();
            startActivity(i);
            return;
        }
        if (activeTab != TAB_SINGLE) return;
        // melee from the selected map (imported file or a map bundled inside the MPQ archives)
        if (selectedMap == null && selectedMpqMap == null) {
            Toast.makeText(this, S.t("Pasirinkite žemėlapį."), Toast.LENGTH_LONG).show();
            return;
        }
        int race = spSlotRace(0);
        // Remember the chosen race so the profile stats can bucket a "Random"
        // win under R (the engine only reports the resolved concrete race).
        prefs().edit().putInt("lastLocalRace", race).apply();
        Intent i = new Intent(this, OpenBWActivity.class);
        if (selectedMpqMap != null) i.putExtra(OpenBWActivity.EXTRA_MAP_MPQ, selectedMpqMap);
        else i.putExtra(OpenBWActivity.EXTRA_MAP, selectedMap.getAbsolutePath());
        i.putExtra(OpenBWActivity.EXTRA_RACE, race);
        i.putExtra(OpenBWActivity.EXTRA_ENEMY_RACE, prefs().getInt("enemyRace", 5));
        i.putExtra(OpenBWActivity.EXTRA_PLAYERS, prefs().getInt("playerCount", 2));
        i.putExtra(OpenBWActivity.EXTRA_TEAM_MODE, prefs().getInt("teamMode", 0));
        // Per-player races and colours drive the local slot and each AI opponent.
        i.putExtra(OpenBWActivity.EXTRA_RACES, spRacesCsv());
        i.putExtra(OpenBWActivity.EXTRA_COLORS, spColorsCsv());
        addCommonExtras(i);
        i.putExtra("qol", qolBits());      // SCBW2 house rules
        // v0.35: bundled UMS scenarios play by their own map triggers/victory,
        // not melee rules - override the game mode (overwrites addCommonExtras).
        if (selectedMpqMap != null && selectedMpqMap.contains("/scenario/")) {
            i.putExtra(OpenBWActivity.EXTRA_GAME_MODE, "ums");
        }
        startActivity(i);
    }

    // SCBW2 house-rules bitmask from the launcher checkboxes (bit0 multi-building
    // select; more bits as each rule's sim side is wired). 0 == vanilla BW.
    private int qolBits() {
        int b = 0;
        if (prefs().getBoolean("qol_grouprally", true))  b |= 1 << 1; // group rally (UI-fan, replay-safe)
        if (prefs().getBoolean("qol_mbs", true))         b |= 1 << 2; // MBS train/research fan-out (UI-fan, replay-safe)
        if (prefs().getBoolean("qol_unlimitedsel", true)) b |= 1 << 3; // unlimited selection + SC2 sub-cards (UI-fan, replay-safe)
        if (prefs().getBoolean("qol_smartcast", true))   b |= 1 << 4; // smart-cast best-energy caster (UI-fan, replay-safe)
        if (prefs().getBoolean("qol_autoworker", false)) b |= 1 << 5; // auto-worker (recorded inject, SP; default OFF)
        if (prefs().getBoolean("qol_workerrally", true)) b |= 1 << 6; // split worker/unit rally (SP-only: changes rally-on-resource sim, so native gates it off for replay/net)
        // bit0 multi_building_select (persistent sim multi-selection): a recorded
        // select action keeps every building. Safe since P5 (v0.101.001) -- the .rep
        // trailer stores game_state::qol and playback restores it. Default OFF.
        if (prefs().getBoolean("qol_multiselect", false)) b |= 1;
        return b;
    }

    // v0.33: launch a campaign mission directly (shared by START and the
    // post-victory "Continue" progression).
    private void launchCampaignMission(String campaignDir) {
        if (!mpqMissing().isEmpty()) {
            Toast.makeText(this, S.t("Pirma importuokite MPQ failus."), Toast.LENGTH_LONG).show();
            return;
        }
        // v0.79 item 1: remember which episode this mission came from, so the
        // launcher returns to that mission list rather than dumping the player
        // back on Single Player - which is where recreate() lands by default.
        prefs().edit().putString("returnToEpisode", episodeTitleFor(campaignDir)).apply();
        Intent ci = new Intent(this, OpenBWActivity.class);
        ci.putExtra(OpenBWActivity.EXTRA_CAMPAIGN, campaignDir);
        addCommonExtras(ci);
        startActivity(ci);
    }

    // Which episode contains this mission directory; empty if none does (a
    // campaign map launched some other way).
    private String episodeTitleFor(String campaignDir) {
        for (Episode e : campaignEpisodes())
            for (String[] m : e.missions)
                if (m[1].equals(campaignDir)) return e.title;
        return "";
    }

    // v0.83.015: land on the episode's mission list after a mission, from
    // BOTH onCreate (activity was recreated) and onResume (activity survived
    // in the background -- the common case on a high-RAM tablet like the S8U,
    // where onCreate does NOT run on return, which is why the player kept
    // landing on Single Player). One-shot: the pref is cleared once consumed.
    private boolean returnToReplayIfPending() {
        String path = prefs().getString("returnToReplay", "");
        if (path.isEmpty()) return false;
        prefs().edit().remove("returnToReplay").apply();
        File rep = new File(path);
        if (!rep.exists()) return false;
        selectTab(TAB_REPLAY);
        selectReplayFile(rep);
        return true;
    }

    private boolean returnToEpisodeIfPending() {
        String backTo = prefs().getString("returnToEpisode", "");
        if (backTo.isEmpty()) return false;
        prefs().edit().remove("returnToEpisode").apply();
        for (Episode e : campaignEpisodes()) {
            if (e.title.equals(backTo)) {
                selectTab(TAB_KAMPANIJA);
                openEpisodeDetail(e);
                return true;
            }
        }
        return false;
    }

    private void addCommonExtras(Intent i) {
        i.putExtra(OpenBWActivity.EXTRA_VOLUME, prefs().getInt("volume", 50));
        i.putExtra(OpenBWActivity.EXTRA_MUSIC_VOLUME, prefs().getInt("musicVolume", 40));
        i.putExtra(OpenBWActivity.EXTRA_RENDER_SCALE, prefs().getInt("renderScale", 2));
        i.putExtra(OpenBWActivity.EXTRA_SHOW_QUEUE, prefs().getBoolean("showQueue", true));
        i.putExtra(OpenBWActivity.EXTRA_GAME_SPEED, prefs().getInt("gameSpeed", 100));
        // (Faze 6: the LEGACY-UI and RGBA-sprites toggles were retired; getArguments()
        // now hard-codes --ui-legacy 0 / --rgba-sprites 1, so no extras are sent.)
        String mods = enabledModsCsv();
        if (!mods.isEmpty()) i.putExtra(OpenBWActivity.EXTRA_MODS, mods);
        i.putExtra(OpenBWActivity.EXTRA_GAME_MODE, prefs().getString("gameMode", "melee"));
        i.putExtra(OpenBWActivity.EXTRA_VS_AI, prefs().getBoolean("vsAi", false));
        i.putExtra(OpenBWActivity.EXTRA_PLAYER_NAME, activeProfile()); // UserID = in-game "Name"
        if (prefs().getBoolean("useBot", false)) {
            java.io.File botsDir = new java.io.File(getFilesDir(), "bots");
            java.io.File[] libs = botsDir.isDirectory()
                    ? botsDir.listFiles((d, n) -> n.endsWith(".so")) : null;
            String name = selectedBotName(libs);
            if (name != null) {
                i.putExtra(OpenBWActivity.EXTRA_BOT, new java.io.File(botsDir, name).getAbsolutePath());
                // The built-in AI's difficulty (0..3) rides along; the bot reads it
                // through the host's difficulty() query. Other bots ignore it.
                if (name.equals("libunifiedai.so")) i.putExtra("botLevel", prefs().getInt("botLevel", 2));
            }
        }
    }

    // The bot .so the user picked (files/bots/<name>.so), else the built-in AI if
    // present, else the first available one. null when none exist.
    private String selectedBotName(java.io.File[] libs) {
        if (libs == null || libs.length == 0) return null;
        String sel = prefs().getString("selectedBot", "");
        for (java.io.File f : libs) if (f.getName().equals(sel)) return sel;
        for (java.io.File f : libs) if (f.getName().equals("libunifiedai.so")) return f.getName();
        return libs[0].getName();
    }

    // A human label for a bot .so: "Name (race)" from its mod.json manifest
    // sidecar (files/bots/<id>.json, schema openbw-bot/1), else the filename.
    // The sidecar is installed next to the .so by unzipMod when a bot-mod is
    // downloaded/imported; a hand-pushed .so with no manifest shows its filename.
    private String botDisplay(java.io.File so) {
        String fn = so.getName();
        String bare = fn.endsWith(".so") ? fn.substring(0, fn.length() - 3) : fn;
        String[] tries = bare.startsWith("lib")
                ? new String[]{ bare.substring(3), bare } : new String[]{ bare };
        for (String t : tries) {
            java.io.File mf = new java.io.File(so.getParentFile(), t + ".json");
            if (!mf.isFile()) continue;
            try {
                java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                try (java.io.FileInputStream in = new java.io.FileInputStream(mf)) {
                    byte[] buf = new byte[4096]; int n;
                    while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
                }
                org.json.JSONObject j = new org.json.JSONObject(new String(bo.toByteArray(), "UTF-8"));
                String name = j.optString("name", "").trim();
                String race = j.optString("race", "").trim();
                if (name.isEmpty()) continue;
                return race.isEmpty() ? name : name + " (" + race + ")";
            } catch (Exception ignore) {}
        }
        return fn;
    }

    private SharedPreferences prefs() {
        return getSharedPreferences("openbw", MODE_PRIVATE);
    }

    // --- v0.26 user profiles (UserID) --------------------------------------
    // A profile is the player's display name: shown as "Name" in melee and LAN
    // and owning its own campaign progress. Shared settings (audio, resolution,
    // HUD) stay global - they are device, not player, properties.
    private String activeProfile() {
        String p = prefs().getString("activeProfile", "");
        if (p.isEmpty()) { ensureDefaultProfile(); p = prefs().getString("activeProfile", "Player"); }
        return p;
    }
    private void ensureDefaultProfile() {
        SharedPreferences pr = prefs();
        if (!pr.getString("activeProfile", "").isEmpty()) return;
        java.util.Set<String> set = new java.util.HashSet<>();
        set.add("Player");
        SharedPreferences.Editor e = pr.edit();
        e.putStringSet("profiles", set);
        e.putString("activeProfile", "Player");
        // One-time migration: fold the old global campaign progress into the
        // default profile so existing saves survive the switch to profiles.
        if (pr.contains("campaignDir")) e.putString("p.Player.campaignDir", pr.getString("campaignDir", ""));
        if (pr.contains("campaignDone")) e.putStringSet("p.Player.campaignDone", pr.getStringSet("campaignDone", new java.util.HashSet<String>()));
        e.apply();
    }
    private String cDirKey() { return "p." + activeProfile() + ".campaignDir"; }
    private String cDoneKey() { return "p." + activeProfile() + ".campaignDone"; }
    private String getCampaignDir() { return prefs().getString(cDirKey(), ""); }
    private void setCampaignDir(String v) { prefs().edit().putString(cDirKey(), v).apply(); }
    private java.util.Set<String> getCampaignDone() {
        return prefs().getStringSet(cDoneKey(), new java.util.HashSet<String>());
    }
    private void setCampaignDone(java.util.Set<String> s) { prefs().edit().putStringSet(cDoneKey(), s).apply(); }

    // --- v0.31 profile statistics -----------------------------------------
    // Games played + wins per race, counted for melee-vsAI and LAN games only
    // (campaign wins are tracked via campaignDone, not here). A random race is
    // recorded as the resolved race the engine picked. Keys are per-profile.
    private static final String[] RACE_KEY = {"Z", "T", "P"};
    private static final String[] RACE_NAME = {"Zerg", "Terran", "Protoss"};
    private String statKey(String suffix) { return "p." + activeProfile() + ".stat." + suffix; }
    private int getStat(String suffix) { return prefs().getInt(statKey(suffix), 0); }
    private void addStat(String suffix, int delta) {
        prefs().edit().putInt(statKey(suffix), getStat(suffix) + delta).apply();
    }
    // Reads melee-result.txt (written by the native side on a melee/LAN game
    // over), updates the active profile's stats, and deletes it. Returns true
    // if a result was consumed. Format: "won=<0|1> race=<0|1|2|5> mode=<ai|lan|local>".
    private boolean consumeMeleeResult() {
        try {
            java.io.File f = new java.io.File(getExternalFilesDir(null), "melee-result.txt");
            if (!f.isFile()) return false;
            byte[] b = new byte[(int) f.length()];
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            int n = in.read(b);
            in.close();
            f.delete();
            String s = n > 0 ? new String(b, 0, n).trim() : "";
            boolean won = false; int race = 5; String mode = "";
            for (String tok : s.split("\\s+")) {
                if (tok.startsWith("won=")) won = tok.endsWith("1");
                else if (tok.startsWith("race=")) { try { race = Integer.parseInt(tok.substring(5)); } catch (Exception e) {} }
                else if (tok.startsWith("mode=")) mode = tok.substring(5);
            }
            android.util.Log.i("OpenBW", "melee-result consumed: won=" + won + " race=" + race + " mode=" + mode);
            // Only melee-vsAI (incl. bots) and LAN count toward statistics.
            if (!mode.equals("ai") && !mode.equals("lan")) return false;
            addStat("games", 1);
            // Bucket the win by the race the player CHOSE: a "Random" pick counts
            // as R even though the engine reports the resolved concrete race.
            int chosen = prefs().getInt("lastLocalRace", race);
            String bucket = (chosen >= 0 && chosen <= 2) ? RACE_KEY[chosen] : "R";
            addStat("games." + bucket, 1);   // per-race games: the profile card shows wins / played
            if (won) addStat("wins." + bucket, 1);
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }

    // Launcher redesign F1-F2: the profile is an ANIMATED unit portrait (decoded from
    // the user's own MPQs) + the name; tapping it opens the profile card, which owns
    // rename / switch / new and the portrait gallery (ProfileUi).
    private ProfileUi profileUi;
    private ProfileUi profileUi() {
        if (profileUi == null) profileUi = new ProfileUi(this, new ProfileUi.Host() {
            public SharedPreferences prefs() { return SetupActivity.this.prefs(); }
            public String activeProfile() { return SetupActivity.this.activeProfile(); }
            public File gameDir() { return getExternalFilesDir(null); }
            public int playerColor() { return PLAYER_COLORS[spSlotColor(0) % PLAYER_COLORS.length]; }
            public List<String[]> campaignProgress() {
                List<String[]> out = new ArrayList<>();
                java.util.Set<String> done = getCampaignDone();
                for (Episode ep : campaignEpisodes()) {
                    int beaten = 0;
                    for (String[] m : ep.missions) if (done.contains(m[1])) beaten++;
                    out.add(new String[]{ep.title, String.valueOf(beaten), String.valueOf(ep.missions.length)});
                }
                return out;
            }
            public void reload() { recreate(); }
        });
        return profileUi;
    }

    // ---- the rail (LAUNCHER-REDESIGN 3.1) ----------------------------------------
    // Screen names. Keys are the Lithuanian originals (S.t translates at display time).
    private static final String[] SCREEN_TITLE = {
        "Vienas žaidėjas", "Tinklas (LAN)", "Kampanija", "Modai", "Replay'ai", "Išsaugoti žaidimai", "Nustatymai"};

    private View buildRail(boolean wide) {
        LinearLayout rail = new LinearLayout(this);
        rail.setOrientation(LinearLayout.VERTICAL);
        rail.setPadding(dp(10), dp(10), dp(10), dp(10));
        final File gameDir = getExternalFilesDir(null);
        int pid = PortraitStore.activeId(prefs(), activeProfile(), gameDir);
        // What the native side draws for "me" (loading card, score file): the id shown
        // here, so random mode stays consistent for the whole launcher session.
        if (prefs().getInt("activePortrait", -1) != pid) prefs().edit().putInt("activePortrait", pid).apply();

        // profile block: the animated portrait IS the profile (tap -> profile card)
        LinearLayout block = new LinearLayout(this);
        block.setOrientation(LinearLayout.VERTICAL);
        final PortraitView portrait = new PortraitView(this);
        int innerW = dp(wide ? 236 : 260) - dp(20);
        int hPx = getResources().getDisplayMetrics().heightPixels;
        portrait.setScale(Math.max(1, Math.min(PortraitView.scaleFor(innerW - dp(4)), (int) (hPx * 0.30f) / 56)));
        portrait.setBorderColor(PLAYER_COLORS[spSlotColor(0) % PLAYER_COLORS.length]);
        portrait.setPortrait(gameDir, pid);
        block.addView(portrait);
        TextView name = kit.text(activeProfile(), Color.WHITE, 18);
        name.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        name.setPadding(0, dp(8), 0, 0);
        block.addView(name);
        TextView stats = kit.text(S.t("Žaidimų ") + getStat("games") + "  ·  " + S.t("Pergalės")
                + "  Z " + getStat("wins.Z") + "  T " + getStat("wins.T") + "  P " + getStat("wins.P") + "  R " + getStat("wins.R"), Kit.TEXT2, 11);
        stats.setPadding(0, dp(2), 0, dp(10));
        block.addView(stats);
        block.setOnClickListener(v -> { portrait.speak(); profileUi().showProfileCard(); });
        rail.addView(block);

        // the seven screens, in three groups: play / my things / settings
        java.util.Set<String> done = getCampaignDone();
        int beaten = 0, missions = 0;
        for (Episode ep : campaignEpisodes()) for (String[] m : ep.missions) { ++missions; if (done.contains(m[1])) ++beaten; }
        String mods = enabledModsCsv();
        int modCount = mods == null || mods.isEmpty() ? 0 : mods.split(",").length;
        addRailEntry(rail, TAB_SINGLE, null);
        addRailEntry(rail, TAB_MULTI, null);
        addRailEntry(rail, TAB_KAMPANIJA, missions > 0 ? beaten + "/" + missions : null);
        rail.addView(railSeparator());
        addRailEntry(rail, TAB_SAVE, countFiles(new File(gameDir, "saves"), ".sav"));
        addRailEntry(rail, TAB_REPLAY, countFiles(new File(gameDir, "replays"), ".rep"));
        addRailEntry(rail, TAB_MODAI, modCount > 0 ? String.valueOf(modCount) : null);
        rail.addView(railSeparator());
        addRailEntry(rail, TAB_NUSTATYMAI, null);

        rail.addView(new View(this), new LinearLayout.LayoutParams(1, 0, 1f));

        // file status + version
        rail.addView(railStatus(true, "MPQ 1.16.1"));
        boolean scr = !prefs().getString("scrCascDir", "").isEmpty();
        String tier = prefs().getString("gfxTier", "hd2");
        rail.addView(railStatus(scr, scr ? "SC:R · " + ("full".equals(tier) ? "FULL HD" : "sd".equals(tier) ? "SD" : "HD2")
                : S.t("SC:R nepasirinkta · SD")));
        final TextView mute = kit.text("", Kit.TEXT2, 11);
        final Runnable muteText = () -> mute.setText("\u266A  " + (prefs().getBoolean("menuMute", false)
                ? S.t("meniu garsas nutildytas · įjungti") : S.t("meniu garsas įjungtas · nutildyti")));
        muteText.run();
        mute.setPadding(0, dp(5), 0, dp(5));
        mute.setOnClickListener(v -> {
            prefs().edit().putBoolean("menuMute", !prefs().getBoolean("menuMute", false)).apply();
            MenuAudio.apply();
            muteText.run();
        });
        rail.addView(mute);
        TextView ver = kit.text("open1161HD v" + versionName(), Kit.TEXT2, 10);
        ver.setPadding(0, dp(4), 0, 0);
        rail.addView(ver);

        ScrollView sv = new ScrollView(this);     // short screens (phones in landscape): the rail scrolls
        sv.setFillViewport(true);
        sv.setBackground(kit.box(Kit.PANEL, Kit.LINE));
        sv.addView(rail, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return sv;
    }

    private String countFiles(File dir, String ext) {
        int n = 0;
        File[] fs = dir.listFiles();
        if (fs != null) for (File f : fs) {
            if (f.isDirectory()) {
                File[] sub = f.listFiles();
                if (sub != null) for (File g : sub) if (g.isFile() && g.getName().toLowerCase(Locale.ROOT).endsWith(ext)) ++n;
            } else if (f.getName().toLowerCase(Locale.ROOT).endsWith(ext)) ++n;
        }
        return n > 0 ? String.valueOf(n) : null;
    }

    private void addRailEntry(LinearLayout rail, final int tab, String count) {
        LinearLayout e = new LinearLayout(this);
        e.setOrientation(LinearLayout.HORIZONTAL);
        e.setGravity(Gravity.CENTER_VERTICAL);
        e.setMinimumHeight(dp(46));
        View mark = new View(this);
        e.addView(mark, new LinearLayout.LayoutParams(dp(3), ViewGroup.LayoutParams.MATCH_PARENT));
        TextView t = kit.text(S.t(SCREEN_TITLE[tab]).toUpperCase(Locale.ROOT), Kit.TEXT2, 14);
        t.setLetterSpacing(0.05f);
        t.setPadding(dp(10), dp(12), dp(6), dp(12));
        e.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (count != null) {
            TextView c = kit.text(count, Kit.TEXT2, 11);
            c.setBackground(kit.box(0x00000000, Kit.STEEL));
            c.setPadding(dp(6), dp(1), dp(6), dp(1));
            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            clp.rightMargin = dp(8);
            e.addView(c, clp);
        }
        e.setOnClickListener(v -> { MenuAudio.play(MenuAudio.CLICK); selectTab(tab); });
        railEntries[tab] = e;
        rail.addView(e, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void styleRailEntry(int tab, boolean on) {
        if (railEntries == null || railEntries[tab] == null) return;
        LinearLayout e = railEntries[tab];
        e.getChildAt(0).setBackgroundColor(on ? Kit.LABEL : 0x00000000);
        if (on) {
            GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[]{0xFF14401F, 0x40143C1E});
            e.setBackground(g);
        } else e.setBackground(null);
        TextView t = (TextView) e.getChildAt(1);
        t.setTextColor(on ? Color.WHITE : 0xFF9FB3A4);
    }

    private View railSeparator() {
        View v = new View(this);
        v.setBackgroundColor(Kit.LINE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1)));
        lp.topMargin = dp(8);
        lp.bottomMargin = dp(8);
        lp.leftMargin = dp(8);
        lp.rightMargin = dp(8);
        v.setLayoutParams(lp);
        return v;
    }

    private View railStatus(boolean ok, String text) {
        TextView t = kit.text("\u25CF  " + text, Kit.TEXT2, 11);
        android.text.SpannableString sp = new android.text.SpannableString(t.getText());
        sp.setSpan(new android.text.style.ForegroundColorSpan(ok ? Kit.LABEL : Kit.DANGER), 0, 1, 0);
        t.setText(sp);
        t.setPadding(0, dp(1), 0, dp(1));
        return t;
    }

    // One line left of the primary action: what START will launch.
    private void updateActionSummary() {
        if (actionSummary == null) return;
        if (activeTab == TAB_MULTI) {
            boolean inLobby = mpLobbyBox != null && mpLobbyBox.getVisibility() == View.VISIBLE;
            if (inLobby && mpState != null) {
                int waiting = 0;
                for (LanLobby.Slot sl : mpState.slots) if (!sl.ready) ++waiting;
                actionSummary.setText(mpState.slots.size() < 2 ? S.t("Laukiama žaidėjų…")
                        : waiting == 0 ? S.t("Visi pasiruošę.") : S.t("Laukiama pasirengimo: ") + waiting);
            } else if (mpSub == 0) {
                String m = selectedMpqMap != null ? (selectedMpqMapName != null ? selectedMpqMapName : mapBasename(selectedMpqMap))
                        : selectedMap != null ? selectedMap.getName() : S.t("žemėlapis nepasirinktas");
                actionSummary.setText(clampCount(prefs().getInt("playerCount", 2)) + " " + S.t("žaidėjai") + "  ·  " + m
                        + "  ·  " + S.t("greitis: ") + gameSpeedName(String.valueOf(prefs().getInt("gameSpeed", 100))));
            } else {
                actionSummary.setText(mpSelHost == null ? S.t("Pasirink hostą sąraše.")
                        : S.t("Pasirinktas hostas: ") + (mpSelHost.name.isEmpty() ? mpSelHost.ip : mpSelHost.name) + "  ·  " + mapBasename(mpSelHost.map));
            }
            return;
        }
        if (activeTab == TAB_MODAI) {
            actionSummary.setText(selectedMod == null ? "" : selectedMod.name + "  ·  "
                    + (selectedMod.active ? S.t("aktyvus") : selectedMod.installed ? S.t("įdiegtas") : S.t("talpykloje")));
            return;
        }
        if (activeTab == TAB_SAVE) {
            actionSummary.setText(selectedSave == null ? "" : saveDisplayTitle(selectedSave));
            return;
        }
        if (activeTab == TAB_REPLAY) {
            File rp = selectedReplay != null && selectedReplay.exists() ? selectedReplay : null;
            actionSummary.setText(rp == null ? "" : rp.getName());
            return;
        }
        if (activeTab == TAB_KAMPANIJA) {
            boolean inMissions = campaignMissionBox != null && campaignMissionBox.getVisibility() == View.VISIBLE
                    && campaignDetailEp != null && campaignSelIdx >= 0 && campaignSelIdx < campaignDetailEp.missions.length;
            String dir = inMissions ? campaignDetailEp.missions[campaignSelIdx][1] : getCampaignDir();
            actionSummary.setText(dir.isEmpty() ? S.t("Pasirink epizodą.") : campaignDisplayName(dir));
            return;
        }
        if (activeTab != TAB_SINGLE) { actionSummary.setText(""); return; }
        int count = clampCount(prefs().getInt("playerCount", 2));
        String map = selectedMpqMap != null ? (selectedMpqMapName != null ? selectedMpqMapName : mapBasename(selectedMpqMap))
                : selectedMap != null ? selectedMap.getName() : S.t("žemėlapis nepasirinktas");
        String opp = opponentKindLabel();
        actionSummary.setText(count + " " + S.t("žaidėjai") + "  ·  " + map + "  ·  " + opp);
    }

    // Game mode (v0.20): melee (default), UMS (map's own triggers/units) or a
    // built-in .trg variant (ctf / greed / slaughter / sudden death).
    private static final String[] GAME_MODE_LABELS = {"Melee", "UMS", "CTF", "Greed 10k", "Slaughter 30", "Sudden Death"};
    private static final String[] GAME_MODE_VALUES = {"melee", "ums", "ctf", "greed10000", "slaughter30", "suddendeath"};


    // --- v0.56 MOD manager (VCMI-style) ----------
    // One unified list of local (files/mods/*) and repository (index.json)
    // mods with status icons, a type column (from mod.json), search + filter,
    // and a drill-in detail pane with a single contextual action button.
    private static final class ModEntry {
        String id, name, type, version = "", author = "", desc = "";
        java.util.List<String> depends = new ArrayList<>();
        java.util.List<String> provides = new ArrayList<>();   // S1.2: layer categories
        int format = 0, engineMin = 0, engineMax = 0;
        boolean executable;
        boolean installed, active;
        String repoUrl, repoVersion;             // repository side (nullable)
        org.json.JSONObject repoJson;
        /**
         * v0.83.009 (S1.2): SEMANTIC comparison. The old test was
         * `!repoVersion.equals(version)`, which called a downgrade an update and
         * ranked "1.10" below "1.9" -- string order is not version order. VCMI
         * hit the same wall and solved it the same way (CModVersion::fromString).
         */
        boolean updatable() {
            return installed && compareVersions(repoVersion, version) > 0;
        }
    }

    /** -1 / 0 / +1 comparing dotted numeric versions; missing parts count as 0. */
    static int compareVersions(String a, String b) {
        if (a == null) a = "";
        if (b == null) b = "";
        if (a.isEmpty() || b.isEmpty()) return 0;   // unknown is not "newer"
        String[] pa = a.split("[.+-]"), pb = b.split("[.+-]");
        int n = Math.max(pa.length, pb.length);
        for (int i = 0; i < n; ++i) {
            long va = 0, vb = 0;
            if (i < pa.length) try { va = Long.parseLong(pa[i].trim()); } catch (Exception ignored) {}
            if (i < pb.length) try { vb = Long.parseLong(pb[i].trim()); } catch (Exception ignored) {}
            if (va != vb) return va < vb ? -1 : 1;
        }
        return 0;
    }

    /**
     * S1.2 compatibility gate: is this mod meant for THIS engine build?
     * Returns null when it fits, otherwise the reason to show the user.
     * Deliberately checked BEFORE anything is installed or activated -- the
     * VCMI lesson is that compatibility settled after parsing is settled too
     * late, and a mod that installs but silently does nothing is worse than one
     * that refuses with a sentence.
     */
    private String modIncompatibleReason(ModEntry e) {
        int vc = versionCodeOf();
        if (e.engineMin > 0 && vc < e.engineMin) {
            return S.t("Modui reikia naujesnės programos versijos (versionCode ≥ ") + e.engineMin + ")";
        }
        if (e.engineMax > 0 && vc > e.engineMax) {
            return S.t("Modas skirtas senesnei programos versijai (versionCode ≤ ") + e.engineMax + ")";
        }
        return null;
    }

    private int versionCodeOf() {
        try {
            android.content.pm.PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            return (int) (android.os.Build.VERSION.SDK_INT >= 28
                    ? pi.getLongVersionCode() : pi.versionCode);
        } catch (Exception e) {
            return 0;
        }
    }
    private LinearLayout modListBox, modDetailBox;
    private String selectedModId;
    private EditText modSearch;
    private int modFilter = 0; // 0=Visi 1=Aktyvūs 2=Įdiegti 3=Talpykloje
    private final java.util.List<org.json.JSONObject> repoCache = new ArrayList<>();

    private ModEntry selectedMod;   // the entry behind selectedModId (for the action bar)

    private void buildModPage(LinearLayout root) {
        // Launcher redesign F7 (LAUNCHER-REDESIGN 5.9): toolbar (search, filter, reload,
        // imports, repository) over a list | detail pair; the contextual action
        // (Download / Update / Activate / Disable / Remove) lives in the action bar.
        Kit.Flow bar = new Kit.Flow(this, dp(8));
        modSearch = new EditText(this);
        modSearch.setHint(S.t("Paieška…"));
        modSearch.setSingleLine(true);
        modSearch.setTextColor(Kit.TEXT);
        modSearch.setHintTextColor(Kit.TEXT2);
        modSearch.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        modSearch.setBackground(kit.box(0xFF05080A, Kit.STEEL));
        modSearch.setPadding(dp(10), 0, dp(10), 0);
        modSearch.addTextChangedListener(new android.text.TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) { renderModList(); }
            public void afterTextChanged(android.text.Editable s) {}
        });
        bar.addView(modSearch, new ViewGroup.LayoutParams(dp(220), dp(44)));
        bar.addView(kit.segmented(new String[]{S.t("Visi"), S.t("Aktyvūs"), S.t("Įdiegti"), S.t("Talpykloje")}, modFilter,
                idx -> { modFilter = idx; renderModList(); }), new ViewGroup.LayoutParams(dp(360), dp(44)));
        bar.addView(kit.smallButton(S.t("Atnaujinti sąrašą").toUpperCase(Locale.ROOT), this::reloadRepo), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44)));
        bar.addView(kit.smallButton(S.t("+ Modas (.zip)").toUpperCase(Locale.ROOT), () -> pickFiles(REQ_PICK_MOD)), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44)));
        bar.addView(kit.smallButton(S.t("+ Botas (.so)").toUpperCase(Locale.ROOT), () -> pickFiles(REQ_PICK_BOT)), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44)));
        // The repository address is a one-time setup (it also lives in Settings).
        bar.addView(kit.smallButton(S.t("Talpykla…").toUpperCase(Locale.ROOT), this::configureRepo), new ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44)));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.bottomMargin = dp(8);
        root.addView(bar, blp);

        LinearLayout[] ld = listDetailPage(root, dp(52));
        modListBox = ld[0];
        modDetailBox = ld[1];
        renderModList();
        showModPlaceholder();
        // v1.0.21: the list is fetched by itself the first time the section opens
        // (quietly: no toast when the device is offline)
        if (repoCache.isEmpty() && !repoAutoTried) { repoAutoTried = true; repoQuiet = true; reloadRepo(); }
    }
    private boolean repoAutoTried = false, repoQuiet = false;

    private void showModPlaceholder() {
        selectedMod = null;
        emptyDetail(modDetailBox, S.t("Pasirink modą iš sąrašo kairėje."));
        updateStartButtonVisibility();
    }

    // Re-read from disk and re-select, so the list and the detail pane always
    // agree after an activate / uninstall / download.
    private void selectMod(String id) {
        selectedModId = id;
        renderModList();
        ModEntry found = null;
        if (id != null) for (ModEntry e : loadMods()) if (e.id.equals(id)) { found = e; break; }
        if (found != null) showModDetail(found); else { selectedModId = null; showModPlaceholder(); }
    }

    // Local mods (files/mods/*) merged with the cached repository index by id.
    private java.util.List<ModEntry> loadMods() {
        java.util.LinkedHashMap<String, ModEntry> byId = new java.util.LinkedHashMap<>();
        File modsDir = new File(getExternalFilesDir(null), "mods");
        File[] dirs = modsDir.listFiles(File::isDirectory);
        if (dirs != null) {
            java.util.Arrays.sort(dirs, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File d : dirs) {
                ModEntry e = new ModEntry();
                e.id = d.getName();
                e.installed = true;
                e.active = prefs().getBoolean("mod_" + e.id, false);
                readModJson(new File(d, "mod.json"), e);
                // A mod with no mod.json at all (both of the packs in use today)
                // still gets its categories from what is on disk.
                if (e.provides.isEmpty()) e.provides.addAll(detectProvides(d));
                if (e.name == null) e.name = e.id;
                if (e.type == null) e.type = "Nežinomas";
                byId.put(e.id, e);
            }
        }
        for (org.json.JSONObject m : repoCache) {
            String id = m.optString("id", "");
            if (id.isEmpty()) continue;
            ModEntry e = byId.get(id);
            if (e == null) { e = new ModEntry(); e.id = id; byId.put(id, e); }
            e.repoUrl = m.optString("url", "");
            e.repoVersion = m.optString("version", "");
            e.repoJson = m;
            if (e.name == null || e.name.equals(e.id)) e.name = m.optString("name", e.id);
            if (e.type == null || e.type.equals("Nežinomas")) e.type = m.optString("type", "Mod");
            if (e.desc == null || e.desc.isEmpty()) e.desc = m.optString("description", "");
            if (e.author == null || e.author.isEmpty()) e.author = m.optString("author", "");
            // The repository index carries the same v1 fields, so a mod can be
            // refused BEFORE it is downloaded rather than after it is unpacked.
            if (e.engineMin == 0) e.engineMin = m.optInt("engine_min", 0);
            if (e.engineMax == 0) e.engineMax = m.optInt("engine_max", 0);
            if (e.format == 0) e.format = m.optInt("format", 0);
            if (!e.executable) e.executable = m.optBoolean("executable", false);
            if (e.provides.isEmpty()) {
                org.json.JSONArray pr = m.optJSONArray("provides");
                if (pr != null) for (int i = 0; i < pr.length(); i++) {
                    String s = pr.optString(i, "");
                    if (!s.isEmpty()) e.provides.add(s);
                }
            }
        }
        return new ArrayList<>(byId.values());
    }

    /**
     * What a mod actually provides, read off disk. The category names are the
     * engine's own directories -- android_main.cpp attaches exactly these
     * (data.mpq / data / anim / ui / icons / tileset / bots), so a mod whose
     * folder is named anything else provides nothing, which is the single most
     * common way a mod "does not work".
     */
    private java.util.List<String> detectProvides(File dir) {
        java.util.List<String> out = new ArrayList<>();
        if (dir == null || !dir.isDirectory()) return out;
        if (new File(dir, "anim").isDirectory()) out.add("anim");
        if (new File(dir, "ui").isDirectory()) out.add("ui");
        if (new File(dir, "icons").isDirectory()) out.add("icons");
        if (new File(dir, "tileset").isDirectory()) out.add("tileset");
        if (new File(dir, "data").isDirectory() || new File(dir, "data.mpq").isFile()) out.add("data");
        if (new File(dir, "bots").isDirectory()) out.add("bots");
        // v0.83.033: a mod may ship a "maps/" folder of .scm/.scx. Unlike the
        // engine-attached categories above, maps are NOT wired into the native
        // side -- the launcher simply lists an active map-pack's files in the
        // map picker (see populateMapList / activeModMaps), so a map pack is
        // "download -> activate -> maps appear", and deactivate hides them.
        if (new File(dir, "maps").isDirectory()) out.add("maps");
        return out;
    }

    /**
     * v0.83.033: .scm/.scx files inside every ACTIVE mod's maps/ folder. This is
     * what makes an activated map-pack mod show up in the map lists. Only active
     * mods are scanned, so toggling a mod off hides its maps again.
     */
    private java.util.List<File> activeModMaps() {
        java.util.List<File> out = new ArrayList<>();
        File modsRoot = new File(getExternalFilesDir(null), "mods");
        File[] mods = modsRoot.listFiles();
        if (mods == null) return out;
        for (File mod : mods) {
            if (!mod.isDirectory()) continue;
            if (!prefs().getBoolean("mod_" + mod.getName(), false)) continue;
            File[] ms = new File(mod, "maps").listFiles((d, n) -> {
                String l = n.toLowerCase(Locale.ROOT);
                return l.endsWith(".scm") || l.endsWith(".scx");
            });
            if (ms != null) for (File m : ms) out.add(m);
        }
        return out;
    }

    /**
     * Locate a map by its file NAME across files/maps/ and every active mod's
     * maps/ folder. Used so a LAN join-by-id and hasMap() find map-pack maps too,
     * not just directly-imported ones. Returns null if nothing matches.
     */
    private File resolveMapFile(String name) {
        File direct = new File(new File(getExternalFilesDir(null), "maps"), name);
        if (direct.exists()) return direct;
        for (File m : activeModMaps()) if (m.getName().equals(name)) return m;
        return null;
    }

    private void readModJson(File f, ModEntry e) {
        if (!f.isFile()) return;
        try {
            byte[] b = new byte[(int) f.length()];
            try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
                int off = 0, n; while (off < b.length && (n = in.read(b, off, b.length - off)) > 0) off += n;
            }
            org.json.JSONObject o = new org.json.JSONObject(new String(b, "UTF-8"));
            e.name = o.optString("name", e.id);
            e.type = o.optString("type", "Mod");
            e.version = o.optString("version", "");
            e.author = o.optString("author", "");
            e.desc = o.optString("description", "");
            org.json.JSONArray dep = o.optJSONArray("depends");
            if (dep != null) for (int i = 0; i < dep.length(); i++) e.depends.add(dep.optString(i));
            // v0.83.009 (S1.2) mod.json v1. Every field is optional: a mod
            // written before this format still loads, it simply declares
            // nothing and is gated by nothing.
            e.format = o.optInt("format", 0);
            e.engineMin = o.optInt("engine_min", 0);
            e.engineMax = o.optInt("engine_max", 0);
            e.executable = o.optBoolean("executable", false);
            org.json.JSONArray pr = o.optJSONArray("provides");
            if (pr != null) for (int i = 0; i < pr.length(); i++) {
                String s = pr.optString(i, "");
                if (!s.isEmpty()) e.provides.add(s);
            }
            if (e.provides.isEmpty()) e.provides.addAll(detectProvides(f.getParentFile()));
        } catch (Exception ignored) {}
    }

    private void renderModList() {
        if (modListBox == null) return;
        modListBox.removeAllViews();
        String q = modSearch != null ? modSearch.getText().toString().trim().toLowerCase(Locale.ROOT) : "";
        java.util.List<ModEntry> mods = loadMods();
        int shown = 0;
        for (ModEntry e : mods) {
            if (modFilter == 1 && !e.active) continue;
            if (modFilter == 2 && !e.installed) continue;
            if (modFilter == 3 && e.installed) continue; // repo-only
            if (!q.isEmpty() && !(e.name.toLowerCase(Locale.ROOT).contains(q)
                    || e.type.toLowerCase(Locale.ROOT).contains(q))) continue;
            modListBox.addView(modCard(e));
            shown++;
        }
        android.util.Log.i("openbw-mods", "renderModList: repoCache=" + repoCache.size()
                + " merged=" + mods.size() + " shown=" + shown + " filter=" + modFilter);
        if (shown == 0) modListBox.addView(emptyLabel(mods.isEmpty()
                ? S.t("(nėra modų)")
                : S.t("(nė vienas modas neatitinka filtro)")));
    }

    // Status glyph: ✓ active · ○ installed-inactive · ⟳ update · ↓ downloadable.
    private String modStatusIcon(ModEntry e) {
        if (e.updatable()) return "⟳";
        if (e.active) return "✓";
        if (e.installed) return "○";
        return "↓";
    }
    private int modStatusColor(ModEntry e) {
        if (e.updatable()) return 0xFFE0B040;
        if (e.active) return 0xFF66D080;
        if (e.installed) return 0xFF9AB0C0;
        return 0xFF7FB8D8;
    }

    // v1.0.21: a mod's thumbnail -- the index entry's "thumb" (an address relative to the
    // repository, like "url"). Fetched once, kept in the cache directory and in memory.
    private final java.util.HashMap<String, android.graphics.Bitmap> modThumbs = new java.util.HashMap<>();
    private View modThumb(final ModEntry e, int sizeDp) {
        if (e.repoJson == null) return null;
        final String t = e.repoJson.optString("thumb", "");
        if (t.isEmpty()) return null;
        final android.widget.ImageView iv = new android.widget.ImageView(this);
        iv.setScaleType(android.widget.ImageView.ScaleType.CENTER_CROP);
        iv.setBackgroundColor(0xFF10161A);
        final String key = e.id + ":" + t;
        android.graphics.Bitmap have = modThumbs.get(key);
        if (have != null) { iv.setImageBitmap(have); return iv; }
        final String url = t.startsWith("http") ? t : modRepoUrl() + "/" + t;
        final File cached = new File(getCacheDir(), "thumb-" + e.id.replaceAll("[^A-Za-z0-9._-]", "_") + "-" + Integer.toHexString(t.hashCode()));
        new Thread(() -> {
            try {
                if (!cached.exists() || cached.length() == 0) {
                    java.net.HttpURLConnection c = openRepoConn(url, "image/*");
                    try (InputStream in = c.getInputStream(); java.io.FileOutputStream out = new java.io.FileOutputStream(cached)) {
                        byte[] buf = new byte[16384];
                        int n; long got = 0;
                        while ((n = in.read(buf)) > 0) { out.write(buf, 0, n); got += n; if (got > (2 << 20)) throw new java.io.IOException("thumbnail too large"); }
                    }
                }
                final android.graphics.Bitmap bmp = android.graphics.BitmapFactory.decodeFile(cached.getPath());
                if (bmp == null) { cached.delete(); return; }
                new Handler(Looper.getMainLooper()).post(() -> { modThumbs.put(key, bmp); iv.setImageBitmap(bmp); });
            } catch (Exception ex) {
                cached.delete();
                android.util.Log.w("openbw-mods", "thumbnail " + url + ": " + ex);
            }
        }, "modthumb").start();
        return iv;
    }

    private View modCard(final ModEntry e) {
        boolean sel = e.id.equals(selectedModId);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(10), dp(8), dp(10), dp(8));
        row.setMinimumHeight(dp(52));
        row.setBackground(sel ? kit.box(Kit.SEL_BG, Kit.LABEL) : kit.box(0xFF070B0E, e.active ? 0xFF2F5A3A : 0xFF2A3338));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(6);
        row.setLayoutParams(lp);
        TextView icon = kit.text(modStatusIcon(e), modStatusColor(e), 18);
        icon.setMinWidth(dp(34));
        row.addView(icon);
        View thumb = modThumb(e, 40);
        if (thumb != null) {
            LinearLayout.LayoutParams tl = new LinearLayout.LayoutParams(dp(40), dp(40));
            tl.rightMargin = dp(10);
            row.addView(thumb, tl);
        }
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        TextView name = kit.text(e.name, Color.WHITE, 15);
        name.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        name.setSingleLine(true);
        name.setEllipsize(android.text.TextUtils.TruncateAt.END);
        col.addView(name);
        String v = e.version != null && !e.version.isEmpty() ? " · v" + e.version : "";
        col.addView(kit.text(e.type + v, Kit.TEXT2, 12));
        row.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (e.executable) {
            TextView g = kit.text(S.t("vykdomas kodas"), Kit.ACCENT, 11);
            g.setBackground(kit.box(0x00000000, Kit.ACCENT));
            g.setPadding(dp(6), dp(1), dp(6), dp(1));
            row.addView(g);
        }
        TextView st = kit.text(e.updatable() ? S.t("yra atnaujinimas") + " → v" + e.repoVersion
                : e.active ? S.t("aktyvus") : e.installed ? S.t("įdiegtas") : S.t("talpykloje"), Kit.TEXT2, 12);
        st.setPadding(dp(10), 0, 0, 0);
        row.addView(st);
        row.setOnClickListener(v2 -> selectMod(e.id));
        return row;
    }

    private void showModDetail(ModEntry e) {
        selectedMod = e;
        modDetailBox.removeAllViews();
        TextView h = kit.text(e.name, Color.WHITE, 20);
        h.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        modDetailBox.addView(h);

        StringBuilder mb = new StringBuilder(e.type);
        if (e.version != null && !e.version.isEmpty()) mb.append(" · v").append(e.version);
        if (e.author != null && !e.author.isEmpty()) mb.append(" · ").append(e.author);
        TextView meta = kit.text(mb.toString(), Kit.TEXT2, 13);    // the state is on the list row and the action button
        meta.setPadding(0, dp(6), 0, dp(8));
        modDetailBox.addView(meta);
        View bigThumb = modThumb(e, 140);
        if (bigThumb != null) {
            LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(dp(140), dp(140));
            bl.bottomMargin = dp(10);
            modDetailBox.addView(bigThumb, bl);
        }

        TextView desc = kit.text(e.desc != null && !e.desc.isEmpty() ? e.desc : S.t("(nėra aprašymo)"), Kit.TEXT, 14);
        desc.setPadding(0, 0, 0, dp(10));
        modDetailBox.addView(desc);

        // What this mod actually replaces, in the engine's own category names. Visible
        // because "it installed but nothing changed" is almost always a category the
        // engine never looked at.
        if (!e.provides.isEmpty()) {
            StringBuilder ps = new StringBuilder();
            for (int i = 0; i < e.provides.size(); ++i) {
                if (i > 0) ps.append(", ");
                ps.append(e.provides.get(i));
            }
            modDetailBox.addView(detailRow(S.t("Teikia"), ps.toString()));
        }
        modDetailBox.addView(detailRow(S.t("Priklausomybės"), e.depends.isEmpty() ? "—" : android.text.TextUtils.join(", ", e.depends)));
        if (e.executable) {
            TextView ex = kit.text(S.t("⚠ Šiame mode yra vykdomo kodo (botų)"), Kit.ACCENT, 13);
            ex.setPadding(0, dp(10), 0, 0);
            modDetailBox.addView(ex);
        }
        updateStartButtonVisibility();
    }

    private void modDownload(ModEntry e) {
        if (e.repoJson != null) downloadMod(modRepoUrl(), e.repoJson);
    }
    private void modToggle(ModEntry e) {
        if (e.active) {
            prefs().edit().putBoolean("mod_" + e.id, false).apply();
            selectMod(e.id);
            return;
        }
        activateMod(e);
    }

    /**
     * v0.83.009 (S1.3): activation with two gates.
     *
     * 1. COMPATIBILITY, checked first: a mod declaring an engine range outside
     *    this build is refused with the reason, instead of switching on and
     *    doing nothing.
     * 2. CATEGORY CONFLICT: anim / ui / icons / tileset are EXCLUSIVE layers in
     *    the engine -- android_main.cpp keeps one directory per category, so a
     *    second mod providing the same category does not blend in, it loses
     *    silently. That silence is the "my mod did not turn on" bug the user
     *    hit; now the launcher names the mod already holding the category and
     *    offers to swap. (`data/` is layered and never conflicts.)
     */
    private void activateMod(ModEntry e) {
        String why = modIncompatibleReason(e);
        if (why != null) {
            new AlertDialog.Builder(this)
                    .setTitle(S.t("Nesuderinamas modas"))
                    .setMessage(e.name + "\n\n" + why)
                    .setPositiveButton(S.t("Gerai"), null)
                    .show();
            return;
        }
        java.util.LinkedHashMap<String, ModEntry> clash = new java.util.LinkedHashMap<>();
        for (ModEntry other : loadMods()) {
            if (other.id.equals(e.id) || !other.active) continue;
            for (String cat : other.provides) {
                if (EXCLUSIVE_CATEGORIES.contains(cat) && e.provides.contains(cat)) {
                    clash.put(cat, other);
                }
            }
        }
        if (clash.isEmpty()) {
            prefs().edit().putBoolean("mod_" + e.id, true).apply();
            selectMod(e.id);
            return;
        }
        StringBuilder msg = new StringBuilder();
        java.util.LinkedHashSet<String> toDisable = new java.util.LinkedHashSet<>();
        for (java.util.Map.Entry<String, ModEntry> c : clash.entrySet()) {
            msg.append("• ").append(c.getKey()).append(": ").append(c.getValue().name).append('\n');
            toDisable.add(c.getValue().id);
        }
        new AlertDialog.Builder(this)
                .setTitle(S.t("Ta pati kategorija jau užimta"))
                .setMessage(S.t("Šias kategorijas jau teikia kitas aktyvus modas:") + "\n\n" + msg
                        + "\n" + S.t("Variklis kiekvienai kategorijai naudoja tik VIENĄ modą. Pakeisti?"))
                .setPositiveButton(S.t("Pakeisti"), (d, w) -> {
                    android.content.SharedPreferences.Editor ed = prefs().edit();
                    for (String id : toDisable) ed.putBoolean("mod_" + id, false);
                    ed.putBoolean("mod_" + e.id, true);
                    ed.apply();
                    selectMod(e.id);
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }

    /** Categories the engine keeps exactly one directory for (android_main.cpp). */
    private static final java.util.Set<String> EXCLUSIVE_CATEGORIES =
            new java.util.HashSet<>(java.util.Arrays.asList("anim", "ui", "icons", "tileset"));

    /**
     * v0.83.011: turn a GitHub release TAG endpoint plus a file name into the
     * API asset URL that can actually be downloaded.
     *
     * Why not put the asset id in the index: a private release asset is only
     * reachable through the API by its numeric id, and that id CHANGES every
     * time the file is replaced. An index full of ids would have to be
     * regenerated on every upload, and a stale one fails with a bare 404. The
     * tag and the file name, on the other hand, are chosen by the author and
     * stay put -- so the id is looked up at download time, once.
     *
     * Runs on the download thread; never call it from the UI thread.
     */
    private String resolveReleaseAsset(String tagUrl, String assetName) throws Exception {
        String js = httpGetText(tagUrl);
        org.json.JSONObject rel = new org.json.JSONObject(js.trim());
        org.json.JSONArray arr = rel.optJSONArray("assets");
        for (int i = 0; arr != null && i < arr.length(); ++i) {
            org.json.JSONObject a = arr.getJSONObject(i);
            if (assetName.equals(a.optString("name"))) {
                String u = a.optString("url", "");
                if (!u.isEmpty()) {
                    android.util.Log.i("openbw-mods", "release asset '" + assetName + "' -> " + u);
                    return u;
                }
            }
        }
        throw new java.io.IOException(S.t("Nerastas failas Release'e: ") + assetName);
    }

    /** SHA-256 of a file, lower-case hex. Matches what `modpack index` prints. */
    private static String sha256Of(File f) throws Exception {
        java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
        try (InputStream in = new java.io.BufferedInputStream(new java.io.FileInputStream(f))) {
            byte[] b = new byte[1 << 16];
            int n;
            while ((n = in.read(b)) > 0) md.update(b, 0, n);
        }
        StringBuilder s = new StringBuilder();
        for (byte x : md.digest()) s.append(String.format("%02x", x));
        return s.toString();
    }

    private Button modActionBtn(String label, Runnable r) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setText(label);
        b.setOnClickListener(v -> r.run());
        return b;
    }
    private void confirmUninstallMod(ModEntry e) {
        new AlertDialog.Builder(this)
                .setTitle(S.t("Pašalinti modą?"))
                .setMessage(e.name + "\nFailai bus ištrinti iš įrenginio.")
                .setPositiveButton(S.t("Pašalinti"), (d, w) -> {
                    deleteRecursive(new File(new File(getExternalFilesDir(null), "mods"), e.id));
                    prefs().edit().remove("mod_" + e.id).apply();
                    Toast.makeText(this, "Pašalinta: " + e.name, Toast.LENGTH_SHORT).show();
                    selectMod(null);
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }
    private void deleteRecursive(File f) {
        if (f == null || !f.exists()) return;
        if (f.isDirectory()) { File[] kids = f.listFiles(); if (kids != null) for (File k : kids) deleteRecursive(k); }
        f.delete();
    }

    // v1.0.20: the project's own mod repository (Cloudflare: a read-only gate in front
    // of a bucket, cloud/mods-gate). Public mods need nothing; private ones need an
    // access code the holder types in -- it is never part of the app. An address the
    // user typed overrides this; the old GitHub address is dropped together with its
    // token, which must not be sent to another host.
    static final String MODS_DEFAULT_URL = "https://mods.openbwhd.workers.dev";
    private String modRepoUrl() {
        String u = prefs().getString("modRepoUrl", "").trim();
        if (u.contains("api.github.com/repos/")) {
            prefs().edit().remove("modRepoUrl").remove("modRepoToken").apply();
            u = "";
        }
        return u.isEmpty() ? MODS_DEFAULT_URL : u;
    }

    // Repository: config the URL, then fetch its index.json into repoCache.
    private void configureRepo() {
        // v0.83.010 (S1.5): address + access token, both stored on THIS device.
        // A private repository needs a token, and the only safe place for it is
        // here -- never in the APK, which anyone can unpack.
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        box.setPadding(pad, pad, pad, 0);

        final EditText input = new EditText(this);
        input.setText(prefs().getString("modRepoUrl", ""));
        input.setHint(MODS_DEFAULT_URL);
        input.setSingleLine(true);
        box.addView(input);

        final EditText tokenIn = new EditText(this);
        tokenIn.setText(prefs().getString("modRepoToken", ""));
        tokenIn.setHint(S.t("Prieigos kodas (privatiems modams; nebūtinas)"));
        tokenIn.setSingleLine(true);
        box.addView(tokenIn);

        TextView note = new TextView(this);
        note.setText(S.t("Kodas lieka tik šiame įrenginyje."));
        note.setTextColor(0xFF7A8A98);
        note.setPadding(0, dp(8), 0, 0);
        box.addView(note);

        new AlertDialog.Builder(this)
                .setTitle(S.t("Modų talpyklos adresas"))
                .setView(box)
                .setPositiveButton(S.t("Išsaugoti + gauti"), (d, w) -> {
                    String url = input.getText().toString().trim();
                    if (url.endsWith("/")) url = url.substring(0, url.length() - 1);
                    prefs().edit()
                            .putString("modRepoUrl", url)
                            .putString("modRepoToken", tokenIn.getText().toString().trim())
                            .apply();
                    reloadRepo();
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }

    private void reloadRepo() {
        String base = modRepoUrl();
        if (base.isEmpty()) { Toast.makeText(this, "Nustatyk talpyklos adresą (mygtukas žemiau).", Toast.LENGTH_SHORT).show(); return; }
        final boolean quiet = repoQuiet;
        repoQuiet = false;
        if (!quiet) Toast.makeText(this, "Gaunamas talpyklos sąrašas…", Toast.LENGTH_SHORT).show();
        final String b = base;
        new Thread(() -> {
            try {
                // v0.83.010 (S1.5): the index is VERSIONED by engine versionCode.
                // An old build asks for a name a new index does not use, so it
                // never sees entries it cannot understand -- compatibility
                // settled before parsing, which is the VCMI lesson. index.json remains as a fallback so
                // a plain unversioned folder still works.
                String json;
                String versioned = b + "/openbw-" + versionCodeOf() + ".json";
                try {
                    json = httpGetText(versioned);
                } catch (Exception first) {
                    json = httpGetText(b + "/index.json");
                }
                final java.util.List<org.json.JSONObject> mods = new ArrayList<>();
                // v0.83.010: strip a UTF-8 BOM before parsing. Not a hypothetical:
                // the index written by PowerShell's `Set-Content -Encoding utf8`
                // starts EF BB BF, JSONObject refuses it, and the failure looks
                // exactly like "repository unreachable" -- which is what the
                // first end-to-end test showed while the server logged 200.
                String trimmed = json.trim();
                if (!trimmed.isEmpty() && trimmed.charAt(0) == 0xFEFF) trimmed = trimmed.substring(1).trim();
                if (trimmed.startsWith("{")) {
                    org.json.JSONObject root = new org.json.JSONObject(trimmed);
                    org.json.JSONArray arr = root.optJSONArray("mods");
                    if (arr == null) throw new java.io.IOException("index has no \"mods\" array");
                    for (int i = 0; i < arr.length(); ++i) mods.add(arr.getJSONObject(i));
                } else {
                    org.json.JSONArray arr = new org.json.JSONArray(trimmed);
                    for (int i = 0; i < arr.length(); ++i) mods.add(arr.getJSONObject(i));
                }
                android.util.Log.i("openbw-mods", "repo index ok: " + mods.size() + " mod(s)");
                new Handler(Looper.getMainLooper()).post(() -> {
                    repoCache.clear();
                    repoCache.addAll(mods);
                    renderModList();
                    if (!quiet) Toast.makeText(this, "Talpykla: " + mods.size() + " modų.", Toast.LENGTH_SHORT).show();
                });
            } catch (Exception e) {
                // v0.83.010: log it. A toast is gone in three seconds and says
                // nothing about WHICH step failed; when this points at a private
                // GitHub repo the difference between "wrong URL", "no token" and
                // "index is not JSON" is the whole diagnosis.
                android.util.Log.w("openbw-mods", "repo fetch failed: " + e, e);
                final String msg = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
                if (!quiet) new Handler(Looper.getMainLooper()).post(() ->
                        Toast.makeText(this, S.t("Talpykla nepasiekiama: ") + msg, Toast.LENGTH_LONG).show());
            }
        }, "modstore").start();
    }

    private void downloadMod(String base, org.json.JSONObject m) {
        final String id = m.optString("id", "").replaceAll("[^A-Za-z0-9._-]", "_");
        String u = m.optString("url", "");
        if (id.isEmpty() || u.isEmpty()) {
            Toast.makeText(this, "Netinkamas talpyklos įrašas.", Toast.LENGTH_LONG).show();
            return;
        }
        final String url = u.startsWith("http") ? u : base + "/" + u;
        // v0.83.011: the index may point at a release TAG endpoint plus the
        // file name instead of a numeric asset id -- resolved below, on the
        // download thread. See resolveReleaseAsset().
        final String assetName = m.optString("asset", "");
        // v0.83.010 (S1.5): refuse an incompatible mod BEFORE spending 250 MB
        // of the user's bandwidth on it.
        int emin = m.optInt("engine_min", 0), emax = m.optInt("engine_max", 0);
        int vc = versionCodeOf();
        if ((emin > 0 && vc < emin) || (emax > 0 && vc > emax)) {
            new AlertDialog.Builder(this)
                    .setTitle(S.t("Nesuderinamas modas"))
                    .setMessage(id + "\n\n" + (emin > 0 && vc < emin
                            ? S.t("Modui reikia naujesnės programos versijos (versionCode ≥ ") + emin + ")"
                            : S.t("Modas skirtas senesnei programos versijai (versionCode ≤ ") + emax + ")"))
                    .setPositiveButton(S.t("Gerai"), null)
                    .show();
            return;
        }
        final String wantSha = m.optString("sha256", "");
        final TextView progressText = new TextView(this);
        int pad = dp(24);
        progressText.setPadding(pad, pad, pad, pad);
        progressText.setTextColor(Color.WHITE);
        progressText.setText(S.t("Siunčiama…"));
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(S.t("Diegiamas modas: ") + id)
                .setView(progressText).setCancelable(false).create();
        dialog.show();
        new Thread(() -> {
            String err = null;
            int files = 0;
            File tmp = new File(getCacheDir(), id + ".zip");
            try {
                String fetchUrl = url;
                if (!assetName.isEmpty() && url.contains("/releases/tags/")) {
                    fetchUrl = resolveReleaseAsset(url, assetName);
                }
                java.net.HttpURLConnection c = openRepoConn(fetchUrl, "application/octet-stream");
                long total = c.getContentLengthLong();
                try (InputStream in = c.getInputStream();
                     java.io.FileOutputStream out = new java.io.FileOutputStream(tmp)) {
                    byte[] buf = new byte[65536];
                    long got = 0;
                    int n;
                    long lastUi = 0;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                        got += n;
                        if (System.currentTimeMillis() - lastUi > 300) {
                            lastUi = System.currentTimeMillis();
                            final String p = total > 0
                                    ? (got * 100 / total) + "%  (" + (got >> 20) + " MB)"
                                    : (got >> 20) + " MB";
                            new Handler(Looper.getMainLooper()).post(() ->
                                    progressText.setText(S.t("Siunčiama… ") + p));
                        }
                    }
                }
                // v0.83.010 (S1.5): verify the bytes before trusting them. The
                // index carries the SHA-256 that `modpack index` computed from
                // the very file that was uploaded, so a truncated download or a
                // swapped asset is caught HERE -- before anything is unpacked,
                // and long before the engine tries to read half an .anim.
                if (!wantSha.isEmpty()) {
                    new Handler(Looper.getMainLooper()).post(() -> progressText.setText(S.t("Tikrinama…")));
                    String got2 = sha256Of(tmp);
                    if (!got2.equalsIgnoreCase(wantSha)) {
                        // Distinguish a truncated/wrong download (byte counts differ) from a
                        // stale index (bytes match the record's size but the sha does not,
                        // i.e. the asset was re-uploaded without refreshing index.json).
                        long gotLen = tmp.length();
                        long wantLen = m.optLong("size", -1);
                        String detail = "sha " + got2.substring(0, 12) + " != " + wantSha.substring(0, 12)
                                + "; bytes " + gotLen + (wantLen >= 0 ? "/" + wantLen : "")
                                + (wantLen >= 0 && gotLen != wantLen ? " (truncated/wrong file)"
                                                                     : " (index sha out of date?)");
                        android.util.Log.w("openbw-mods", "checksum mismatch for " + id + ": " + detail);
                        throw new java.io.IOException(S.t("failas sugadintas, bandyk dar kartą"));   // the digests are in the log
                    }
                }
                new Handler(Looper.getMainLooper()).post(() -> progressText.setText(S.t("Išskleidžiama…")));
                // Same consent rule as the local import path: a downloaded mod
                // carrying .so files asks before its code lands on the device.
                ZipScan dscan = scanModZip(Uri.fromFile(tmp));
                boolean dAllowNative = dscan.nativeLibs.isEmpty()
                        ? false : askNativeConsent(id, dscan.nativeLibs);
                files = unzipMod(Uri.fromFile(tmp), new File(new File(getExternalFilesDir(null), "mods"), id), dAllowNative);
                if (files == 0) err = "tuščias zip";
            } catch (Exception e) {
                err = e.getMessage();
            } finally {
                tmp.delete();
            }
            final String errF = err;
            final int filesF = files;
            new Handler(Looper.getMainLooper()).post(() -> {
                dialog.dismiss();
                if (errF != null) {
                    Toast.makeText(this, "Nepavyko įdiegti: " + errF, Toast.LENGTH_LONG).show();
                } else {
                    Toast.makeText(this, S.t("Įdiegta: ") + id, Toast.LENGTH_LONG).show();
                    if (modListBox != null) selectMod(id); else renderModList();
                }
            });
        }, "moddl").start();
    }

    // Campaign missions (v0.20): CHKs live inside the game MPQs under
    // campaign/<race>/<mission>/staredit/scenario.chk. When one is selected,
    // START GAME (KAMPANIJA tab) launches it (UMS semantics) instead of melee.
    // v0.32: presented as the six original episodes with authentic mission
    // names, each opening its own mission list, to recreate the original SC
    // campaign selection inside our launcher.
    private static final class Episode {
        final String title;
        final String[][] missions; // each = {display name, MPQ dir}
        Episode(String t, String[][] m) { title = t; missions = m; }
    }
    private java.util.List<Episode> campaignEpisodes() {
        java.util.ArrayList<Episode> eps = new java.util.ArrayList<>();
        // --- StarCraft (original) ---
        eps.add(new Episode(S.t("Terran — Rebel Yell (I epizodas)"), new String[][]{
            {"Mokymai — Boot Camp", "campaign/terran/tutorial"},
            {"1. Wasteland", "campaign/terran/terran01"},
            {"2. Backwater Station", "campaign/terran/terran02"},
            {"3. Desperate Alliance", "campaign/terran/terran03"},
            {"4. The Jacobs Installation", "campaign/terran/terran04"},
            {"5. Revolution", "campaign/terran/terran05"},
            {"6. Norad II", "campaign/terran/terran06"},
            {"7. The Trump Card", "campaign/terran/terran07"},
            {"8. The Big Push", "campaign/terran/terran08"},
            {"9. New Gettysburg", "campaign/terran/terran09"},
            {"10. The Hammer Falls", "campaign/terran/terran10"},
        }));
        eps.add(new Episode(S.t("Zerg — Overmind (II epizodas)"), new String[][]{
            {"Mokymai", "campaign/zerg/tutorial"},
            {"1. Among the Ruins", "campaign/zerg/zerg01"},
            {"2. Egression", "campaign/zerg/zerg02"},
            {"3. The New Dominion", "campaign/zerg/zerg03"},
            {"4. Agent of the Swarm", "campaign/zerg/zerg04"},
            {"5. The Amerigo", "campaign/zerg/zerg05"},
            {"6. The Dark Templar", "campaign/zerg/zerg06"},
            {"7. The Culling", "campaign/zerg/zerg07"},
            {"8. Eye for an Eye", "campaign/zerg/zerg08"},
            {"9. The Invasion of Aiur", "campaign/zerg/zerg09"},
            {"10. Full Circle", "campaign/zerg/zerg10"},
        }));
        eps.add(new Episode(S.t("Protoss — The Fall (III epizodas)"), new String[][]{
            {"Mokymai", "campaign/protoss/tutorial"},
            {"1. First Strike", "campaign/protoss/protoss01"},
            {"2. Into the Flames", "campaign/protoss/protoss02"},
            {"3. Higher Ground", "campaign/protoss/protoss03"},
            {"4. The Hunt for Tassadar", "campaign/protoss/protoss04"},
            {"5. Choosing Sides", "campaign/protoss/protoss05"},
            {"6. Into the Darkness", "campaign/protoss/protoss06"},
            {"7. Homeland", "campaign/protoss/protoss07"},
            {"8. The Trial of Tassadar", "campaign/protoss/protoss08"},
            {"9. Shadow Hunters", "campaign/protoss/protoss09"},
            {"10. Eye of the Storm", "campaign/protoss/protoss10"},
        }));
        // --- Brood War ---
        eps.add(new Episode("Brood War: Protoss — The Stand (IV)", new String[][]{
            {"1. Escape from Aiur", "campaign/expprotoss/protoss01"},
            {"2. Dunes of Shakuras", "campaign/expprotoss/protoss02"},
            {"3. Legacy of the Xel'Naga", "campaign/expprotoss/protoss03"},
            {"4. The Quest for Uraj", "campaign/expprotoss/protoss04"},
            {"5. The Battle of Braxis", "campaign/expprotoss/protoss05"},
            {"6. Return to Char", "campaign/expprotoss/protoss06"},
            {"7. The Insurgent", "campaign/expprotoss/protoss07"},
            {"8. Countdown", "campaign/expprotoss/protoss08"},
        }));
        eps.add(new Episode("Brood War: Terran — The Iron Fist (V)", new String[][]{
            {"1. First Strike", "campaign/expterran/terran01"},
            {"2. The Dylarian Shipyards", "campaign/expterran/terran02"},
            {"3. Ruins of Tarsonis", "campaign/expterran/terran03"},
            {"4. Assault on Korhal", "campaign/expterran/terran04"},
            {"5a. Emperor's Fall (I)", "campaign/expterran/terran05a"},
            {"5b. Emperor's Fall (II)", "campaign/expterran/terran05b"},
            {"6. Emperor's Flight", "campaign/expterran/terran06"},
            {"7. Patriot's Blood", "campaign/expterran/terran07"},
            {"8. To Chain the Beast", "campaign/expterran/terran08"},
        }));
        eps.add(new Episode("Brood War: Zerg — Queen of Blades (VI)", new String[][]{
            {"1. Vile Disruption", "campaign/expzerg/zerg01"},
            {"2. Reign of Fire", "campaign/expzerg/zerg02"},
            {"3. The Kel-Morian Combine", "campaign/expzerg/zerg03"},
            {"4a. The Liberation of Korhal (a)", "campaign/expzerg/zerg04a"},
            {"4b. The Liberation of Korhal (b)", "campaign/expzerg/zerg04b"},
            {"4c. The Liberation of Korhal (c)", "campaign/expzerg/zerg04c"},
            {"4d. The Liberation of Korhal (d)", "campaign/expzerg/zerg04d"},
            {"4e. The Liberation of Korhal (e)", "campaign/expzerg/zerg04e"},
            {"4f. The Liberation of Korhal (f)", "campaign/expzerg/zerg04f"},
            {"5. True Colors", "campaign/expzerg/zerg05"},
            {"6. Fury of the Swarm", "campaign/expzerg/zerg06"},
            {"7. Drawing of the Web", "campaign/expzerg/zerg07"},
            {"8. To Slay the Beast", "campaign/expzerg/zerg08"},
            {"9. The Reckoning", "campaign/expzerg/zerg09"},
            {"10. Omega", "campaign/expzerg/zerg10"},
            {"★ Dark Origin (slaptoji)", "campaign/expzerg/bonus"},
        }));
        return eps;
    }

    // v0.35 map browser: melee/UMS maps bundled inside the CD MPQ archives
    // (multimaps\...). Each is a .scm/.scx (itself an MPQ); the native side
    // extracts it from the chain to a temp file. Paths use '/' (the MPQ lookup
    // is slash- and case-insensitive). Grouped like campaign episodes.
    private java.util.List<Episode> bundledMapGroups() {
        java.util.ArrayList<Episode> g = new java.util.ArrayList<>();
        g.add(new Episode(S.t("Klasikiniai (SC) — 2–4 žaidėjų"), new String[][]{
            {"(2) Challenger", "multimaps/(2)Challenger.scm"},
            {"(2) River Crossing", "multimaps/(2)River Crossing.scm"},
            {"(2) Space Madness", "multimaps/(2)Space Madness.scm"},
            {"(2) Volcanis", "multimaps/(2)Volcanis.scm"},
            {"(3) Holy Ground", "multimaps/(3)Holy Ground.scm"},
            {"(3) Meltdown", "multimaps/(3)Meltdown.scm"},
            {"(4) Blood Bath", "multimaps/(4)Blood Bath.scm"},
            {"(4) Dark Star", "multimaps/(4)Dark Star.scm"},
            {"(4) Orbital Relay", "multimaps/(4)Orbital Relay.scm"},
            {"(4) Ruins of the Ancients", "multimaps/(4)Ruins of the Ancients.scm"},
        }));
        g.add(new Episode(S.t("Klasikiniai (SC) — 5–8 žaidėjų"), new String[][]{
            {"(5) Diablo", "multimaps/(5)Diablo.scm"},
            {"(5) Sherwood Forest", "multimaps/(5)Sherwood Forest.scm"},
            {"(6) Ground Zero", "multimaps/(6)Ground Zero.scm"},
            {"(6) New Gettysburg", "multimaps/(6)New Gettysburg.scm"},
            {"(7) Continental Divide", "multimaps/(7)Continental Divide.scm"},
            {"(8) Green Valleys", "multimaps/(8)Green Valleys.scm"},
            {"(8) Homeworld", "multimaps/(8)Homeworld.scm"},
            {"(8) The Hunters", "multimaps/(8)The Hunters.scm"},
        }));
        g.add(new Episode("Brood War melee", new String[][]{
            {"(2) Astral Balance", "multimaps/broodwar/(2)Astral Balance.scm"},
            {"(2) Crystallis", "multimaps/broodwar/(2)Crystallis.scm"},
            {"(2) Switchback", "multimaps/broodwar/(2)Switchback.scm"},
            {"(3) Overlook", "multimaps/broodwar/(3)Overlook.scm"},
            {"(3) Triad", "multimaps/broodwar/(3)Triad.scm"},
            {"(4) Caldera", "multimaps/broodwar/(4)Caldera.scm"},
            {"(4) Eye of the Storm", "multimaps/broodwar/(4)Eye of the Storm.scx"},
            {"(4) Nightfall", "multimaps/broodwar/(4)Nightfall.scx"},
            {"(4) Ramparts", "multimaps/broodwar/(4)Ramparts.scm"},
            {"(4) Sanctuary", "multimaps/broodwar/(4)Sanctuary.scx"},
        }));
        g.add(new Episode("UMS scenarijai (mini-žaidimai)", new String[][]{
            {"(2) Pro Bowl", "multimaps/scenario/(2)Pro Bowl.scm"},
            {"(2) Wakka Wakka", "multimaps/scenario/(2)Wakka Wakka.scm"},
            {"(3) Defenders of the Galaxy", "multimaps/scenario/(3)Defenders of the Galaxy.scm"},
            {"(4) King of the Hill", "multimaps/scenario/(4)King of the Hill.scm"},
            {"(4) Old Faithful", "multimaps/scenario/(4)Old Faithful.scm"},
            {"(4) Steal the Bacon", "multimaps/scenario/(4)Steal the Bacon.scm"},
            {"(4) Zergling Round-Up", "multimaps/scenario/(4)Zergling Round-Up.scm"},
            {"(4) Zoo Keeper", "multimaps/scenario/(4)Zoo Keeper.scm"},
            {"(5) Race of Death", "multimaps/scenario/(5)Race of Death.scm"},
            {"(8) StarCraft Fortress", "multimaps/scenario/(8)StarCraft Fortress.scm"},
        }));
        return g;
    }

    private String selectedMpqMap;     // internal MPQ path, or null
    private String selectedMpqMapName; // display name of the bundled selection

    // Maps a saved campaign dir to its human name, or "" if none.
    private String campaignDisplayName(String dir) {
        if (dir == null || dir.isEmpty()) return "";
        for (Episode ep : campaignEpisodes())
            for (String[] m : ep.missions)
                if (m[1].equals(dir)) return m[0];
        return dir;
    }

    private TextView campaignSelTv; // legacy field (kept null; see onActivityResult guards)
    // v0.56 campaign redesign: two drill-in views inside the tab - the episode
    // grid and a per-episode mission list - toggled like the MP lobby.
    private LinearLayout[] pages;
    private LinearLayout campaignEpisodeBox;
    private LinearLayout campaignMissionBox;

    // Episode race: 0=Zerg 1=Terran 2=Protoss, inferred from the mission path
    // (campaign/{terran,zerg,protoss,expterran,...}/...).
    private int episodeRace(Episode ep) {
        String p = ep.missions.length > 0 ? ep.missions[0][1] : "";
        if (p.contains("zerg")) return 0;
        if (p.contains("protoss")) return 2;
        return 1; // terran / default
    }
    private static final int[] RACE_CARD_BG  = {0xFF34204A, 0xFF1E3A4A, 0xFF3A3418}; // Z/T/P
    private static final int[] RACE_CARD_ACC = {0xFFB060D0, 0xFF4A90C8, 0xFFD0B040};
    private static final String[] RACE_LABEL = {"Zerg", "Terran", "Protoss"};

    // v0.91 campaign redesign: the mission list is a two-column master/detail view
    // (list left, description + START right) instead of the old single list + a
    // briefing AlertDialog. Curated per-mission descriptions (keyed by MPQ dir,
    // the stable id) are wrapped in S.t() at bind time so they stay translatable;
    // the in-game briefing already lives in the engine, so it is not duplicated
    // here. Pure launcher UI -- sim-core / determinism gates untouched.
    private Episode campaignDetailEp;
    private int campaignSelIdx = -1;
    private int campaignDetailRace;
    private boolean campaignDetailWide;
    private View[] campaignRows;
    private boolean[] campaignRowUnlocked, campaignRowCurrent;
    private TextView detailTitle, detailDesc, detailStatus;
    private Button startMissionBtn;

    private static java.util.Map<String, String> MISSION_DESC;
    private static synchronized java.util.Map<String, String> missionDescMap() {
        if (MISSION_DESC != null) return MISSION_DESC;
        java.util.HashMap<String, String> m = new java.util.HashMap<>();
        // --- Episode I: Terran, Rebel Yell ---
        m.put("campaign/terran/tutorial", "Training mission. Learn the basics of moving units, gathering minerals, and building a Terran base under the guidance of your adjutant.");
        m.put("campaign/terran/terran01", "As magistrate of the Mar Sara colony you're ordered by Alpha Squadron's Edmund Duke to relocate colonists. Establish a base and hold the wasteland against the first alien intrusions.");
        m.put("campaign/terran/terran02", "A distress call comes from Backwater Station, overrun by the same alien organisms. Jim Raynor leads the Colonial Militia to defend the outpost and drive the creatures back.");
        m.put("campaign/terran/terran03", "Abandoned by the Confederacy, the colony must survive on its own. Hold out against relentless Zerg waves until evacuation, with the rebel Sons of Korhal watching.");
        m.put("campaign/terran/terran04", "Now allied with Arcturus Mengsk's Sons of Korhal, infiltrate a locked-down Confederate research facility. Fight through indoor corridors to seize the classified data hidden inside.");
        m.put("campaign/terran/terran05", "Rescue Sarah Kerrigan and rally the colonists of Antiga Prime to the rebellion. Defend the uprising against Confederate forces while Mengsk turns the world to his cause.");
        m.put("campaign/terran/terran06", "General Duke has crash-landed deep in Zerg territory and is broadcasting a rescue signal. Break through and keep the downed Norad II battlecruiser alive until Duke is secured.");
        m.put("campaign/terran/terran07", "With Duke defected to the rebels, deploy captured Confederate psi-emitter technology. Protect the emitters and the base while Mengsk's master plan takes shape.");
        m.put("campaign/terran/terran08", "Launch a full offensive against the entrenched Confederate military. Build a strong army and grind down the enemy strongholds blocking the road to Tarsonis.");
        m.put("campaign/terran/terran09", "Assault the Confederate capital world of Tarsonis alongside the Zerg. Clear the defenders from the field as Mengsk's ruthless endgame unfolds.");
        m.put("campaign/terran/terran10", "The newly crowned Dominion turns on its own. Survive the final confrontation and break free as Jim Raynor severs ties with the tyrant Mengsk.");
        // --- Episode II: Zerg, Overmind ---
        m.put("campaign/zerg/tutorial", "Training mission. Learn to command the Zerg Swarm — spawning larvae, spreading creep, and morphing your first strains.");
        m.put("campaign/zerg/zerg01", "As a newborn Cerebrate you take your first steps on ravaged Tarsonis. Learn the ways of the Swarm and consume the remnants of terran resistance.");
        m.put("campaign/zerg/zerg02", "The captured Sarah Kerrigan is being infested. Protect the chrysalis and the hive cluster from human raiders until her transformation is complete.");
        m.put("campaign/zerg/zerg03", "Mengsk's fledgling Dominion moves against the Swarm. Crush the terran forces and prove the Zerg's dominance over the sector's new emperor.");
        m.put("campaign/zerg/zerg04", "The reborn Kerrigan joins the Swarm as its deadliest agent. Support her strikes against terran and protoss forces contesting the brood's territory.");
        m.put("campaign/zerg/zerg05", "Board the derelict science vessel Amerigo to seize a captive dark templar's secrets. A close-quarters installation mission with no base — every unit counts.");
        m.put("campaign/zerg/zerg06", "A dark templar has slain a Cerebrate, revealing a lethal threat to the Swarm. Hunt down the intruder and defend the brood from further assassinations.");
        m.put("campaign/zerg/zerg07", "A rival brood grows too independent. Purge the rogue Cerebrate Zasz's remnants and reassert the Overmind's absolute will over the Swarm.");
        m.put("campaign/zerg/zerg08", "The Overmind has torn the protoss homeworld's location from a slain enemy's mind. Fend off the vengeful dark templar while the brood prepares to strike.");
        m.put("campaign/zerg/zerg09", "The full invasion begins. Punch through the protoss defenses on Aiur and carve out a foothold for the Swarm on the enemy homeworld.");
        m.put("campaign/zerg/zerg10", "Seize an ancient Xel'Naga temple and its khaydarin crystal at the site where zerg and protoss were first created. Deliver it so the Overmind can manifest on Aiur.");
        // --- Episode III: Protoss, The Fall ---
        m.put("campaign/protoss/tutorial", "Training mission. Learn to command the Protoss — warping in structures via pylon power and channeling the Khala into battle.");
        m.put("campaign/protoss/protoss01", "The Zerg have reached Aiur. As Executor, meet the warrior Fenix at Antioch, destroy the nearby Zerg base, and keep Fenix alive.");
        m.put("campaign/protoss/protoss02", "The exiled Tassadar reveals the Zerg are ruled by Cerebrates. Build your base, hold the line, then join Fenix to slay a Cerebrate and cripple its brood.");
        m.put("campaign/protoss/protoss03", "Defend the templar strongholds as the Zerg press deeper into Aiur. Hold the high ground and blunt the Swarm's advance on the protoss heartland.");
        m.put("campaign/protoss/protoss04", "The Conclave brands Tassadar a traitor for consorting with dark templar. You are ordered to find him and bring him to justice — a command that will test your loyalty.");
        m.put("campaign/protoss/protoss05", "The rift with the Conclave breaks open. Side with Tassadar and the dark templar Zeratul, and disable the Conclave's forces sent to stop you.");
        m.put("campaign/protoss/protoss06", "Venture into shadow to prove the dark templar's power. Guide Zeratul's strike as a dark templar assassinates a Cerebrate the templar cannot permanently kill.");
        m.put("campaign/protoss/protoss07", "The Conclave's grip is broken and Aiur must be defended anew. Rally the reunited protoss and hold your homeland against a resurgent Zerg tide.");
        m.put("campaign/protoss/protoss08", "The Conclave puts Tassadar on trial and attacks. Rescue him and defeat the Judicator's loyalists to unite the protoss for the final battle.");
        m.put("campaign/protoss/protoss09", "Seek out the dark templar Matriarch and her hidden enclave. Escort the templar through Zerg-held ground to forge the alliance that can end the Overmind.");
        m.put("campaign/protoss/protoss10", "The final assault. With Tassadar, Raynor, and Zeratul at your side, tear through layered Zerg defenses and destroy the Overmind itself.");
        // --- Episode IV (Brood War): Protoss, The Stand ---
        m.put("campaign/expprotoss/protoss01", "Aiur is lost. Lead Zeratul and a small band of survivors — no base, scarce reinforcements — across broken ground to the warp gate and off the doomed world.");
        m.put("campaign/expprotoss/protoss02", "Arriving on the dark templar homeworld Shakuras, the refugees find the Zerg followed them through the gate. Secure a foothold and cleanse the corruption near the city.");
        m.put("campaign/expprotoss/protoss03", "Ancient Xel'Naga ruins hold a temple that could purge the Zerg from Shakuras. Recover the first of two crystals needed to awaken it.");
        m.put("campaign/expprotoss/protoss04", "Journey to reclaim the crystal Uraj from a frozen world. Battle through hostile forces to retrieve the second key to the temple's power.");
        m.put("campaign/expprotoss/protoss05", "A new terran power — the UED — enters the sector. Contest the icy world of Braxis and hold your ground against the Earth fleet's advance.");
        m.put("campaign/expprotoss/protoss06", "Ally uneasily with Kerrigan and strike at the Zerg on Char. Destroy the cerebrate-held broods threatening the fragile pact.");
        m.put("campaign/expprotoss/protoss07", "The Judicator Aldaris rises in revolt against the alliance with Kerrigan. Put down the uprising before it shatters the last of the protoss.");
        m.put("campaign/expprotoss/protoss08", "Activate the Xel'Naga temple with both crystals and survive the countdown. Defend the temple against overwhelming Zerg assaults until it fires.");
        // --- Episode V (Brood War): Terran/UED, The Iron Fist ---
        m.put("campaign/expterran/terran01", "As a captain of Earth's UED expeditionary force, make first landfall in the Koprulu sector. Establish a beachhead and crush the local resistance.");
        m.put("campaign/expterran/terran02", "Raid the Dominion's Dylarian shipyards, where battlecruisers sit in dry-dock. Assault the yards and liberate as many capital ships as you can.");
        m.put("campaign/expterran/terran03", "Search the ruined former Confederate capital for a weapon against the Zerg. Fight through the derelict city to recover the psi-disrupter's data.");
        m.put("campaign/expterran/terran04", "Begin the invasion of Mengsk's throne world. Choose whether to neutralize the Dominion's nukes or its battlecruisers before storming Augustgrad.");
        m.put("campaign/expterran/terran05a", "Storm Augustgrad the ground way — beware Mengsk's ghosts and nuclear silos. Break the Dominion's defenders and topple the emperor's capital.");
        m.put("campaign/expterran/terran05b", "The alternate assault on Augustgrad — beware Mengsk's battlecruiser fleet. Overcome the Dominion's air power and seize the capital.");
        m.put("campaign/expterran/terran06", "Mengsk flees his fallen capital. Pursue the deposed emperor and shatter the escort standing between the UED and total control.");
        m.put("campaign/expterran/terran07", "The UED moves to seize the Overmind itself, but treachery stirs in the ranks. Fight alongside — and against — old allies as the plan turns dark.");
        m.put("campaign/expterran/terran08", "Bend the new Overmind to Earth's will. Escort medics to the beacons ringing the creature and kill the guarding cerebrates to break its defenses.");
        // --- Episode VI (Brood War): Zerg, Queen of Blades ---
        m.put("campaign/expzerg/zerg01", "With her brood scattered, Kerrigan must rebuild. Rescue a chain of isolated hive clusters, each adding reinforcements, before the renegade Zerg destroy them.");
        m.put("campaign/expzerg/zerg02", "The UED's psi-disrupter silences the Swarm. Strike to disable the device and free the broods from Earth's control.");
        m.put("campaign/expzerg/zerg03", "Forge alliances of convenience among the sector's terran factions. Pressure the Kel-Morian holdouts into serving Kerrigan's designs.");
        String korhal = "Part of a six-stage running battle to wrest Korhal from the UED. Each phase shifts the front as Kerrigan and her terran pawns tear the Earth fleet's grip off the throne world.";
        m.put("campaign/expzerg/zerg04a", korhal); m.put("campaign/expzerg/zerg04b", korhal);
        m.put("campaign/expzerg/zerg04c", korhal); m.put("campaign/expzerg/zerg04d", korhal);
        m.put("campaign/expzerg/zerg04e", korhal); m.put("campaign/expzerg/zerg04f", korhal);
        m.put("campaign/expzerg/zerg05", "The alliance of convenience ends. Kerrigan turns on her erstwhile partners in a treacherous strike that reveals her true intentions.");
        m.put("campaign/expzerg/zerg06", "Press the reunited Swarm against the reeling UED. Overrun the Earth forces on Char and drive them toward destruction.");
        m.put("campaign/expzerg/zerg07", "Set the trap for the final confrontation. Seize the key positions that will pin the last of Kerrigan's enemies in place.");
        m.put("campaign/expzerg/zerg08", "The reborn Overmind must die for Kerrigan to reign. Lead the protoss-and-zerg assault to the creature and end its short second life.");
        m.put("campaign/expzerg/zerg09", "Every rival converges for a last stand against the Queen of Blades. Withstand the combined UED, Dominion, and protoss onslaught.");
        m.put("campaign/expzerg/zerg10", "The final battle. Surrounded by the UED, Mengsk's Dominion, and the protoss, destroy every enemy force to cement Kerrigan's dominion over the sector.");
        m.put("campaign/expzerg/bonus", "A hidden bonus mission. A lone protoss force uncovers a strange, ancient secret hinting at powers beyond the war — an eerie epilogue to the Brood War.");
        MISSION_DESC = m;
        return m;
    }
    private String missionDesc(String dir) {
        String d = missionDescMap().get(dir);
        return d == null ? "" : d;
    }
    // Drop a leading "12. " / "5a. " / "★ " mission-number prefix for the big
    // detail-panel title (the list keeps the numbered form).
    private String stripNum(String name) {
        int dot = name.indexOf(". ");
        if (dot > 0 && dot <= 4 && name.substring(0, dot).matches("[0-9]+[a-f]?")) return name.substring(dot + 2);
        if (name.startsWith("★ ")) return name.substring(2);
        return name;
    }

    private void addCampaignRow(LinearLayout root) {
        campaignSelTv = null;
        campaignEpisodeBox = new LinearLayout(this);
        campaignEpisodeBox.setOrientation(LinearLayout.VERTICAL);
        campaignMissionBox = new LinearLayout(this);
        campaignMissionBox.setOrientation(LinearLayout.VERTICAL);
        campaignMissionBox.setVisibility(View.GONE);
        root.addView(campaignEpisodeBox);
        root.addView(campaignMissionBox);
        buildEpisodeGrid();
    }

    // Only Single Player / Replay actually launch through START GAME; the other
    // tabs have their own entry points (Sukurti lobby / Žaisti misiją / mod
    // actions), so the button would just be dead weight there.
    // Extracted from the tab click listener so the launcher can also switch
    // tabs itself (returning to a campaign episode after a mission).
    private void selectTab(int idx) {
        if (pages == null || idx < 0 || idx >= pages.length) return;
        activeTab = idx;
        for (int k = 0; k < pages.length; k++) {
            pages[k].setVisibility(k == idx ? View.VISIBLE : View.GONE);
            styleRailEntry(k, k == idx);
        }
        if (screenTitle != null) {
            screenTitle.setText(S.t(SCREEN_TITLE[idx]).toUpperCase(Locale.ROOT));
            screenSub.setText(idx == TAB_MULTI ? S.t("Mano IP: ") + localIp() : "");   // the one sub-title that is information (join by IP)
        }
        if (drawerScrim != null) drawerScrim.setVisibility(View.GONE);
        updateStartButtonVisibility();
    }

    private void setAction(final TextView b, String label, final Runnable run, boolean enabled) {
        if (b == null) return;
        if (label == null) { b.setVisibility(View.GONE); return; }
        b.setVisibility(View.VISIBLE);
        b.setText(label.toUpperCase(Locale.ROOT));
        b.setOnClickListener(v -> { MenuAudio.play(b == primaryAction ? MenuAudio.LOCK : MenuAudio.CLICK); run.run(); });
        b.setClickable(enabled);
        b.setAlpha(enabled ? 1f : 0.35f);
    }

    // The action bar: what the CURRENT screen (and its state) can do. One primary
    // action; up to two secondary ones on its left. Screens not listed here still
    // carry their buttons inside the page (phase F7 moves them one by one).
    private void updateStartButtonVisibility() {
        if (startButton == null) return;
        boolean show = true;
        if (activeTab == TAB_SINGLE) {
            setAction(tertiaryAction, null, null, false);
            setAction(secondaryAction, S.t("Importuoti žemėlapį"), () -> pickFiles(REQ_PICK_MAP), true);
            setAction(primaryAction, S.t("Pradėti žaidimą"), this::startGame, true);
        } else if (activeTab == TAB_MULTI) {
            boolean inLobby = mpLobbyBox != null && mpLobbyBox.getVisibility() == View.VISIBLE;
            if (inLobby) {
                String ready = mpLocalReady ? "\u2714 " + S.t("Pasiruošęs") : S.t("Pasiruošti");
                if (mpIsHost) {
                    setAction(tertiaryAction, S.t("Palikti"), this::mpLeaveLobby, true);
                    setAction(secondaryAction, ready, this::mpToggleReady, true);
                    setAction(primaryAction, S.t("Pradėti"), () -> { if (mpLobby != null) mpLobby.startGame(); },
                            mpState != null && mpState.allReady());
                } else {
                    setAction(tertiaryAction, null, null, false);
                    setAction(secondaryAction, S.t("Palikti"), this::mpLeaveLobby, true);
                    setAction(primaryAction, ready, this::mpToggleReady, true);
                }
            } else if (mpSub == 0) {
                setAction(tertiaryAction, null, null, false);
                setAction(secondaryAction, S.t("Importuoti žemėlapį"), () -> pickFiles(REQ_PICK_MAP), true);
                setAction(primaryAction, S.t("Sukurti lobby"), this::mpStartHosting, true);
            } else {
                setAction(tertiaryAction, null, null, false);
                setAction(secondaryAction, S.t("Ieškoti / atnaujinti"), this::mpScanHosts, true);
                setAction(primaryAction, S.t("Prisijungti"), () -> { if (mpSelHost != null) mpJoinHost(mpSelHost); },
                        mpSelHost != null && mpHaveMap(mpSelHost.map));
            }
        } else if (activeTab == TAB_MODAI) {
            final ModEntry me = selectedMod;
            if (me == null) {
                setAction(tertiaryAction, null, null, false);
                setAction(secondaryAction, null, null, false);
                setAction(primaryAction, S.t("Aktyvuoti"), () -> {}, false);
            } else if (!me.installed) {
                setAction(tertiaryAction, null, null, false);
                setAction(secondaryAction, null, null, false);
                setAction(primaryAction, S.t("Parsisiųsti"), () -> modDownload(me), true);
            } else {
                setAction(tertiaryAction, S.t("Pašalinti"), () -> confirmUninstallMod(me), true);
                setAction(secondaryAction, me.updatable() ? S.t("Atnaujinti") : null, () -> modDownload(me), true);
                setAction(primaryAction, me.active ? S.t("Išjungti") : S.t("Aktyvuoti"), () -> modToggle(me), true);
            }
        } else if (activeTab == TAB_SAVE) {
            final File sv = selectedSave;
            setAction(tertiaryAction, null, null, false);
            setAction(secondaryAction, S.t("Pašalinti"), () -> { if (sv != null) confirmDeleteSave(sv); }, sv != null);
            setAction(primaryAction, S.t("Tęsti"), () -> { if (sv != null) loadSave(sv); }, sv != null);
        } else if (activeTab == TAB_REPLAY) {
            final File rp = selectedReplay != null && selectedReplay.exists() ? selectedReplay : null;
            boolean last = rp != null && rp.getName().equalsIgnoreCase("LastGame.rep");
            setAction(tertiaryAction, S.t("Pašalinti"), () -> { if (rp != null) confirmDeleteReplay(rp); }, rp != null);
            setAction(secondaryAction, last ? S.t("Išsaugoti") : null, () -> { if (rp != null) keepLastReplay(rp); }, true);
            setAction(primaryAction, S.t("Žiūrėti"), () -> { if (rp != null) { selectedMap = null; startGame(); } },
                    rp != null && replayPlayable(rp));
        } else if (activeTab == TAB_KAMPANIJA) {
            setAction(tertiaryAction, null, null, false);
            setAction(secondaryAction, null, null, false);
            boolean inMissions = campaignMissionBox != null && campaignMissionBox.getVisibility() == View.VISIBLE
                    && campaignDetailEp != null && campaignSelIdx >= 0 && campaignSelIdx < campaignDetailEp.missions.length;
            if (inMissions) {
                final String path = campaignDetailEp.missions[campaignSelIdx][1];
                boolean isDone = getCampaignDone().contains(path);
                setAction(primaryAction, isDone ? S.t("Pakartoti misiją") : S.t("Pradėti misiją"),
                        () -> { setCampaignDir(path); launchCampaignMission(path); }, true);
            } else {
                // the mission the profile stands on (set by the last start / the last victory)
                final String dir = getCampaignDir();
                setAction(primaryAction, S.t("Pradėti misiją"), () -> launchCampaignMission(dir), !dir.isEmpty());
            }
        } else {
            show = false;
        }
        startButton.setVisibility(show ? View.VISIBLE : View.GONE);
        updateActionSummary();
    }

    // ---- Campaign (launcher redesign F7, LAUNCHER-REDESIGN 5.5 / 5.6) -----------------
    private static final String[] EP_NUMERAL = {"I", "II", "III", "IV", "V", "VI"};

    // The episode's animated emblem in the user's CD archives (the original campaign
    // select screen's art). Absent archives -> no emblem, the card still works.
    private String episodeEmblem(Episode ep) {
        boolean bw = ep.missions.length > 0 && ep.missions[0][1].contains("/exp");
        int race = episodeRace(ep);
        String n = race == 0 ? "zerg" : race == 2 ? "prot" : "terr";
        return bw ? "glue\\expcampaign\\x" + n + ".smk" : "glue\\campaign\\" + n + ".smk";
    }
    // "Terran — Rebel Yell (I epizodas)" / "Brood War: Protoss — The Stand (IV)" -> "Rebel Yell"
    private String episodeName(Episode ep) {
        String t = stripEpTag(ep.title);
        int d = t.indexOf(" — ");
        return d >= 0 ? t.substring(d + 3) : t;
    }
    private void loadEmblemInto(final PortraitView v, final String path) {
        final File gameDir = getExternalFilesDir(null);
        minimapExec.execute(() -> {
            final PortraitStore.Clip c = PortraitStore.art(gameDir, path);
            if (c != null) ui.post(() -> v.setArt(c));
        });
    }

    // Six episode cards, three per row (StarCraft I-III above Brood War IV-VI).
    private void buildEpisodeGrid() {
        campaignEpisodeBox.removeAllViews();
        java.util.Set<String> done = getCampaignDone();
        final boolean wide = getResources().getConfiguration().screenWidthDp >= 600;
        java.util.List<Episode> eps = campaignEpisodes();
        LinearLayout grid = new LinearLayout(this);
        grid.setOrientation(LinearLayout.VERTICAL);
        LinearLayout row = null;
        for (int i = 0; i != eps.size(); ++i) {
            Episode ep = eps.get(i);
            int beaten = 0;
            for (String[] m : ep.missions) if (done.contains(m[1])) beaten++;
            View card = episodeCard(ep, i, beaten, wide);
            if (!wide) {
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(170));
                lp.bottomMargin = dp(10);
                grid.addView(card, lp);
                continue;
            }
            if (i % 3 == 0) {
                row = new LinearLayout(this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setBaselineAligned(false);
                LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
                if (i > 0) rlp.topMargin = dp(10);
                grid.addView(row, rlp);
            }
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
            if (i % 3 != 0) lp.leftMargin = dp(10);
            row.addView(card, lp);
        }
        if (wide) {
            campaignEpisodeBox.addView(grid, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(480)));
            fitToPanel(grid);
        } else {
            campaignEpisodeBox.addView(grid);
        }
    }

    private View episodeCard(final Episode ep, int index, int beaten, boolean wide) {
        int race = episodeRace(ep);
        int total = ep.missions.length;
        LinearLayout card = spPanel();
        card.setOrientation(LinearLayout.HORIZONTAL);
        PortraitView emblem = new PortraitView(this);
        loadEmblemInto(emblem, episodeEmblem(ep));
        card.addView(emblem, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.42f));
        LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(dp(8), dp(2), dp(4), dp(2));
        TextView lab = kit.label("Episode " + (index < EP_NUMERAL.length ? EP_NUMERAL[index] : String.valueOf(index + 1)) + " · " + RACE_LABEL[race]);
        lab.setPadding(0, 0, 0, dp(4));
        info.addView(lab);
        TextView title = kit.text(episodeName(ep), Color.WHITE, 18);
        title.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        title.setPadding(0, 0, 0, dp(8));
        info.addView(title);
        info.addView(kit.bar(total > 0 ? (float) beaten / total : 0f, Kit.LABEL),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(6)));
        TextView pt = kit.text(beaten + " / " + total + " " + S.t("misijų"), Kit.TEXT2, 12);
        pt.setPadding(0, dp(6), 0, 0);
        info.addView(pt);
        info.addView(new android.widget.Space(this), new LinearLayout.LayoutParams(1, 0, 1f));
        info.addView(kit.smallButton((beaten >= total && total > 0 ? S.t("Peržaisti") : beaten > 0 ? S.t("Tęsti") : S.t("Pradėti")).toUpperCase(Locale.ROOT),
                () -> openEpisodeDetail(ep)), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40)));
        card.addView(info, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.58f));
        card.setOnClickListener(v -> openEpisodeDetail(ep));
        return card;
    }

    // The per-episode Missions view: left the mission list with linear progression
    // gating, right the selected mission's detail. START MISSION sits in the action
    // bar. Wide screens: both columns fill the panel and scroll on their own; narrow
    // ones stack (the outer page ScrollView handles it).
    private void openEpisodeDetail(Episode ep) {
        java.util.Set<String> done = getCampaignDone();
        boolean unlockAll = prefs().getBoolean("campaignUnlockAll", false);
        int race = episodeRace(ep);
        campaignDetailEp = ep;
        campaignDetailRace = race;
        campaignSelIdx = -1;
        boolean wide = getResources().getConfiguration().screenWidthDp >= 600;
        campaignDetailWide = wide;
        campaignMissionBox.removeAllViews();

        int total = ep.missions.length, beaten = 0;
        for (String[] mm : ep.missions) if (done.contains(mm[1])) beaten++;

        // top row: back + the episode and its progress
        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(0, 0, 0, dp(8));
        top.addView(kit.smallButton(S.t("‹ Epizodai").toUpperCase(Locale.ROOT), this::campaignBackToEpisodes),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44)));
        TextView t = kit.text(ep.title + "  ·  " + beaten + " / " + total, Kit.TEXT2, 13);
        t.setPadding(dp(14), 0, 0, 0);
        top.addView(t);
        campaignMissionBox.addView(top);

        LinearLayout cols = new LinearLayout(this);
        cols.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        cols.setBaselineAligned(false);

        // LEFT: the mission list
        campaignRows = new View[ep.missions.length];
        campaignRowUnlocked = new boolean[ep.missions.length];
        campaignRowCurrent = new boolean[ep.missions.length];
        LinearLayout listCol = new LinearLayout(this);
        listCol.setOrientation(LinearLayout.VERTICAL);
        for (int k = 0; k < ep.missions.length; k++) {
            String path = ep.missions[k][1];
            boolean isDone = done.contains(path);
            boolean unlocked = unlockAll || k == 0 || isDone || done.contains(ep.missions[k - 1][1]);
            boolean current = unlocked && !isDone;
            campaignRowUnlocked[k] = unlocked;
            campaignRowCurrent[k] = current;
            View row = detailMissionRow(ep, k, unlocked, isDone, current, race);
            campaignRows[k] = row;
            listCol.addView(row);
        }
        LinearLayout listPanel = spPanel();
        if (wide) {
            ScrollView sv = new ScrollView(this);
            sv.setNestedScrollingEnabled(true);
            sv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
            sv.addView(listCol);
            listPanel.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        } else {
            listPanel.addView(listCol);
        }

        // RIGHT: the detail panel
        LinearLayout detail = spPanel();
        detail.setPadding(dp(14), dp(12), dp(14), dp(12));
        LinearLayout dhead = new LinearLayout(this);
        dhead.setOrientation(LinearLayout.HORIZONTAL);
        dhead.setGravity(Gravity.CENTER_VERTICAL);
        PortraitView emblem = new PortraitView(this);
        loadEmblemInto(emblem, episodeEmblem(ep));
        dhead.addView(emblem, new LinearLayout.LayoutParams(dp(120), dp(130)));
        LinearLayout dt = new LinearLayout(this);
        dt.setOrientation(LinearLayout.VERTICAL);
        dt.setPadding(dp(12), 0, 0, 0);
        detailTitle = kit.text("", Color.WHITE, 24);
        detailTitle.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        detailTitle.setPadding(0, 0, 0, dp(8));
        dt.addView(detailTitle);
        detailStatus = kit.text("", Kit.ACCENT, 11);
        detailStatus.setPadding(dp(8), dp(2), dp(8), dp(2));
        dt.addView(detailStatus, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        dhead.addView(dt, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        detail.addView(dhead);
        detailDesc = kit.text("", Kit.TEXT, 15);
        detailDesc.setLineSpacing(dp(3), 1f);
        detailDesc.setPadding(0, dp(12), 0, 0);
        if (wide) {
            ScrollView dsv = new ScrollView(this);
            dsv.setNestedScrollingEnabled(true);
            dsv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
            dsv.addView(detailDesc);
            detail.addView(dsv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        } else {
            detail.addView(detailDesc);
        }

        if (wide) {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.42f);
            l1.rightMargin = dp(10);
            cols.addView(listPanel, l1);
            cols.addView(detail, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.58f));
            campaignMissionBox.addView(cols, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(430)));
            fitToPanel(cols, dp(52));
        } else {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            l1.bottomMargin = dp(10);
            cols.addView(listPanel, l1);
            cols.addView(detail, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            campaignMissionBox.addView(cols);
        }

        campaignEpisodeBox.setVisibility(View.GONE);
        campaignMissionBox.setVisibility(View.VISIBLE);

        // Auto-select the current (first not-done unlocked) mission, else the first unlocked.
        int target = -1;
        for (int k = 0; k < ep.missions.length; k++) if (campaignRowCurrent[k]) { target = k; break; }
        if (target < 0) for (int k = 0; k < ep.missions.length; k++) if (campaignRowUnlocked[k]) { target = k; break; }
        selectMission(target < 0 ? 0 : target);
    }

    private View detailMissionRow(Episode ep, int idx, boolean unlocked, boolean isDone, boolean current, int race) {
        String name = ep.missions[idx][0];
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(6);
        row.setLayoutParams(lp);
        row.setPadding(dp(10), dp(8), dp(10), dp(8));
        row.setMinimumHeight(dp(52));

        TextView mark = kit.text(isDone ? "\u2713" : current ? "\u25B6" : unlocked ? "\u2022" : "\uD83D\uDD12",
                isDone ? Kit.LABEL : current ? Kit.ACCENT : Kit.TEXT2, 16);
        mark.setMinWidth(dp(34));
        row.addView(mark);
        // the mark says the state; the detail panel's tag spells it out for the picked one
        TextView tv = kit.text(name, unlocked ? Color.WHITE : Kit.TEXT2, 15);
        tv.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        row.addView(tv);

        final int k = idx;
        row.setOnClickListener(v -> selectMission(k));
        return row;
    }

    // Select a mission on the left and rebind the right panel. Locked rows toast and
    // do not change the selection.
    private void selectMission(int idx) {
        Episode ep = campaignDetailEp;
        if (ep == null || idx < 0 || idx >= ep.missions.length) return;
        if (!campaignRowUnlocked[idx]) {
            Toast.makeText(this,
                    S.t("Užrakinta — pirma įveik ankstesnę misiją."),
                    Toast.LENGTH_LONG).show();
            return;
        }
        campaignSelIdx = idx;
        for (int k = 0; k < campaignRows.length; k++) {
            View row = campaignRows[k];
            if (row == null) continue;
            if (k == idx) row.setBackground(kit.box(Kit.SEL_BG, Kit.LABEL));
            else if (campaignRowCurrent[k]) row.setBackground(kit.box(0xFF070B0E, Kit.ACCENT));
            else row.setBackground(kit.box(0xFF070B0E, 0xFF2A3338));
        }
        bindDetail(idx);
    }

    private void bindDetail(int idx) {
        Episode ep = campaignDetailEp;
        int race = campaignDetailRace;
        final String name = ep.missions[idx][0];
        final String path = ep.missions[idx][1];
        boolean isDone = getCampaignDone().contains(path);
        boolean current = campaignRowCurrent[idx];

        detailTitle.setText(stripNum(name));
        String desc = missionDesc(path);
        if (!desc.isEmpty()) {
            detailDesc.setText(S.t(desc));
        } else {
            // No curated text (should not happen for campaigns) — fall back to the
            // map's own embedded scenario string, loaded off the UI thread.
            detailDesc.setText(S.t("Kraunama…"));
            loadNativeDescInto(path);
        }

        int sc = isDone ? Kit.LABEL : current ? Kit.ACCENT : Kit.TEXT2;
        detailStatus.setText(isDone ? "\u2713 " + S.t("Įveikta") : current ? "\u25B6 " + S.t("Dabartinė misija") : S.t("Prieinama"));
        detailStatus.setTextColor(sc);
        detailStatus.setBackground(kit.box(0x00000000, sc));
        updateStartButtonVisibility();   // START MISSION / REPLAY MISSION for this pick
    }

    // Drop the trailing " (I epizodas)" / " (IV)" tag from an episode title for the
    // detail subheader, which already shows the race.
    private String stripEpTag(String title) {
        int p = title.lastIndexOf(" (");
        return p > 0 ? title.substring(0, p) : title;
    }

    // Async-load a map's embedded scenario description into the detail panel, but
    // only apply it if that mission is still the selected one (guards against a
    // race when the user quickly selects another mission).
    private void loadNativeDescInto(final String path) {
        if (!minimapLibOk) { detailDesc.setText(""); return; }
        final String gameDir = getExternalFilesDir(null).getAbsolutePath();
        final String scratch = new File(getCacheDir(), "mi.tmp").getAbsolutePath();
        minimapExec.execute(() -> {
            String info = null;
            try { info = nativeMapInfoInternal(gameDir, path, scratch); }
            catch (Throwable ignored) {}
            final String desc = parseMapDesc(info);
            ui.post(() -> {
                if (campaignDetailEp != null && campaignSelIdx >= 0
                        && path.equals(campaignDetailEp.missions[campaignSelIdx][1]))
                    detailDesc.setText(desc.isEmpty() ? S.t("(nėra aprašymo šiam žemėlapiui)") : desc);
            });
        });
    }

    private void campaignBackToEpisodes() {
        buildEpisodeGrid();
        campaignMissionBox.setVisibility(View.GONE);
        campaignEpisodeBox.setVisibility(View.VISIBLE);
        campaignSelIdx = -1;
        updateStartButtonVisibility();
    }

    // "name\ndescription" -> description (or the whole string if no newline).
    private String parseMapDesc(String info) {
        if (info == null) return "";
        int nl = info.indexOf('\n');
        String desc = nl >= 0 ? info.substring(nl + 1) : info;
        return desc.trim();
    }

    // Mods: each subdirectory of files/mods/ is a mod. Checked mods are passed to the native side
    // as --mods and their data.mpq / data/ files override the base MPQs.
    private String enabledModsCsv() {
        File modsDir = new File(getExternalFilesDir(null), "mods");
        File[] dirs = modsDir.listFiles(File::isDirectory);
        if (dirs == null) return "";
        java.util.Arrays.sort(dirs, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        StringBuilder sb = new StringBuilder();
        for (File d : dirs) {
            if (!prefs().getBoolean("mod_" + d.getName(), false)) continue;
            if (sb.length() > 0) sb.append(',');
            sb.append(d.getName());
        }
        return sb.toString();
    }

    // ---- Settings (launcher redesign F8, LAUNCHER-REDESIGN 5.10) ----------------------
    // Nine sub-tabs instead of one long page. Every control changes its pref at once
    // (there is no Save); the pref KEYS and DEFAULTS are exactly the ones the old page
    // used -- only the widgets changed (switches, segments, tinted sliders).
    private final List<String> setTabNames = new ArrayList<>();
    private final List<LinearLayout> setTabBoxes = new ArrayList<>();

    private LinearLayout settingsSection(String title) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        setTabNames.add(title);
        setTabBoxes.add(box);
        return box;
    }

    private LinearLayout setRow(String label, String hint) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(7), 0, dp(7));
        row.setMinimumHeight(dp(48));
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(0, 0, dp(12), 0);
        col.addView(kit.text(label, Kit.TEXT, 14));
        if (hint != null && !hint.isEmpty()) col.addView(kit.text(hint, Kit.TEXT2, 11));
        row.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return row;
    }

    // A boolean pref with an explanatory second line.
    private void setSwitch(LinearLayout root, String label, final String key, String hint, boolean def) {
        LinearLayout row = setRow(label, hint);
        final View sw = kit.toggle(prefs().getBoolean(key, def), on -> {
            prefs().edit().putBoolean(key, on).apply();
            MenuAudio.apply();      // menuMusic / menuSounds take effect at once
        });
        row.addView(sw, new LinearLayout.LayoutParams(dp(52), dp(28)));
        row.setOnClickListener(v -> sw.performClick());
        root.addView(row);
    }

    // A 0..max % slider with a live value, storing an int pref.
    private void setSlider(LinearLayout root, String label, String hint, final String key, int def, int max) {
        LinearLayout row = setRow(label, hint);
        int cur = prefs().getInt(key, def);
        final TextView val = kit.text(cur + "%", Kit.ACCENT, 13);
        val.setGravity(Gravity.END);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max);
        bar.setProgress(cur);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(Kit.LABEL));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFF3A464C));
        bar.setThumbTintList(android.content.res.ColorStateList.valueOf(0xFFE0E8E0));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int v, boolean fromUser) {
                val.setText(v + "%");
                prefs().edit().putInt(key, v).apply();
                MenuAudio.apply();  // the menu music follows the volume sliders live
            }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        });
        row.addView(bar, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.3f));
        row.addView(val, new LinearLayout.LayoutParams(dp(52), ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(row);
    }

    // A slider over [min, max] with a unit suffix (the 0..max "%" slider above cannot
    // start anywhere but zero).
    private void setRangeSlider(LinearLayout root, String label, String hint, final String key, int def, final int min, int max, final String unit) {
        LinearLayout row = setRow(label, hint);
        int cur = Math.max(min, Math.min(max, prefs().getInt(key, def)));
        final TextView val = kit.text(cur + unit, Kit.ACCENT, 13);
        val.setGravity(Gravity.END);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(cur - min);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(Kit.LABEL));
        bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFF3A464C));
        bar.setThumbTintList(android.content.res.ColorStateList.valueOf(0xFFE0E8E0));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int v, boolean fromUser) {
                int step = (v + min) / 10 * 10;
                val.setText(step + unit);
                prefs().edit().putInt(key, step).apply();
            }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) {}
        });
        row.addView(bar, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.3f));
        row.addView(val, new LinearLayout.LayoutParams(dp(64), ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(row);
    }

    // A choice among a few labelled values.
    private void setSegments(LinearLayout root, String label, String hint, String[] labels, int sel, Kit.IntSink sink) {
        LinearLayout row = setRow(label, hint);
        row.addView(kit.segmented(labels, sel, sink), new LinearLayout.LayoutParams(0, dp(44), labels.length > 4 ? 2.2f : 1.3f));
        root.addView(row);
    }

    private void setNote(LinearLayout root, String text) {
        TextView t = kit.text(text, Kit.TEXT2, 12);
        t.setPadding(0, dp(4), 0, dp(8));
        root.addView(t);
    }

    private void buildSettingsPage(LinearLayout page) {
        setTabNames.clear();
        setTabBoxes.clear();
        final boolean wide = getResources().getConfiguration().screenWidthDp >= 900;

        // ---- Failai: the game data this app runs on (+ reset) ----
        LinearLayout files = settingsSection(S.t("Failai"));
        LinearLayout mpqRow = setRow("StarCraft: Brood War 1.16.1 (MPQ)", null);
        mpqIndicator = new View(this);
        LinearLayout.LayoutParams indLp = new LinearLayout.LayoutParams(dp(12), dp(12));
        indLp.rightMargin = dp(10);
        mpqRow.addView(mpqIndicator, 0, indLp);
        mpqRow.addView(kit.smallButton(S.t("Importuoti MPQ failus"), () -> pickFiles(REQ_PICK_MPQ)),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40)));
        files.addView(mpqRow);
        mpqStatus = kit.text("", Kit.TEXT2, 12);
        mpqStatus.setPadding(dp(22), 0, 0, dp(10));
        files.addView(mpqStatus);
        LinearLayout scrRow = setRow("StarCraft: Remastered", null);
        scrIndicator = new View(this);
        LinearLayout.LayoutParams sInd = new LinearLayout.LayoutParams(dp(12), dp(12));
        sInd.rightMargin = dp(10);
        scrRow.addView(scrIndicator, 0, sInd);
        scrRow.addView(kit.smallButton(S.t("Pasirinkti SC:R aplanką"), this::pickScrFolder),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40)));
        files.addView(scrRow);
        scrStatus = kit.text("", Kit.TEXT2, 12);
        scrStatus.setPadding(dp(22), 0, 0, dp(10));
        files.addView(scrStatus);
        updateScrIndicator();
        // reset settings (keeps profiles, stats, campaign)
        LinearLayout resetRow = setRow(S.t("Atstatyti numatytus nustatymus"), S.t("Profiliai ir progresas lieka."));
        resetRow.addView(kit.smallButton(S.t("Atstatyti"), () -> new AlertDialog.Builder(this)
                .setTitle(S.t("Atstatyti nustatymus?"))
                .setMessage(S.t("Garsas, grafika, valdymas ir greitis grįš į numatytus. ")
                        + S.t("Profiliai, statistika ir kampanijos progresas NEBUS liesti."))
                .setPositiveButton(S.t("Atstatyti"), (d, w) -> {
                    prefs().edit()
                            .remove("volume").remove("musicVolume").remove("renderScale")
                            .remove("showQueue").remove("gameSpeed")
                            // sound submixes
                            .remove("sfxVolume").remove("voiceVolume").remove("uiVolume").remove("voiceFreq")
                            // HD graphics controls
                            .remove("gfxTier").remove("emissiveGlow").remove("shadowSoft").remove("bloomLevel")
                            .remove("sharpenFilter").remove("sharpenLevel").remove("gradeLevel")
                            .remove("sunShadow").remove("sunShadowLen").remove("sunShadowSign")
                            .remove("pointLights").remove("lightStrength")
                            // interface controls
                            .remove("showFps").remove("showApm").remove("healthBarsAlways").remove("cameraSpeed")
                            .remove("resourceCounts").remove("showSaturation").remove("showZoomButtons").remove("equipGrey")
                            .remove("tapCommand").remove("renderInterp")
                            .remove("hudStyle").remove("hud3Template").remove("hud3Dock").remove("hud3WheelMs").remove("hud3Pen")
                            .remove("hud3Latch").remove("hud3Labels").remove("hud3Left").remove("hud3Scale").remove("hud3Vibrate").remove("hud3AllGroups")
                            .apply();
                    restoreTab = TAB_NUSTATYMAI;
                    recreate();
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show()), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40)));
        files.addView(resetRow);

        // ---- Garsas: master + music, then per-category submixes on top of the master ----
        LinearLayout snd = settingsSection(S.t("Garsas"));
        setSlider(snd, S.t("Garsumas"), null, "volume", 50, 100);
        setSlider(snd, S.t("Muzika"), null, "musicVolume", 40, 100);
        setSlider(snd, S.t("Garso efektai (SFX)"), S.t("Mūšio, judesio ir aplinkos garsai"), "sfxVolume", 100, 100);
        setSlider(snd, S.t("Vienetų balsai"), S.t("Patvirtinimai ir atsakymai („taip, pone\")"), "voiceVolume", 100, 100);
        setSlider(snd, S.t("Sąsajos garsai"), S.t("Pranešimai, mygtukai ir misijų balsai"), "uiVolume", 100, 100);
        setSlider(snd, S.t("Balsų dažnis"), S.t("Kaip dažnai vienetai atsako pažymėjus (100 = kaip originale)"), "voiceFreq", 100, 100);
        // Launcher sound (F9): the original title music and menu clicks, from the user's
        // archives. The music is only in the optional CD archives.
        boolean cdArchives = new File(getExternalFilesDir(null), "StarCraft.mpq").exists()
                || new File(getExternalFilesDir(null), "BroodWar.mpq").exists();
        setSwitch(snd, S.t("Meniu muzika"), "menuMusic", cdArchives ? S.t("Originali meniu muzika.")
                : S.t("Reikia StarCraft.mpq arba BroodWar.mpq (CD archyvų) — be jų meniu tylus."), true);
        setSwitch(snd, S.t("Meniu garsai"), "menuSounds", S.t("Mygtukų ir skydelių garsai."), true);

        // ---- Grafika ----
        LinearLayout gfx = settingsSection(S.t("Grafika"));
        // The actual render resolution (screen px / divisor) per option: lower = sharper
        // but heavier, higher = faster.
        android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        int sw = Math.max(dm.widthPixels, dm.heightPixels), sh = Math.min(dm.widthPixels, dm.heightPixels);
        final int[] scaleVals = {1, 2, 3};
        String[] scaleNames = new String[scaleVals.length];
        int scaleSel = 1;
        for (int i = 0; i < scaleVals.length; ++i) {
            scaleNames[i] = (sw / scaleVals[i]) + "×" + (sh / scaleVals[i]);
            if (scaleVals[i] == prefs().getInt("renderScale", 2)) scaleSel = i;
        }
        setSegments(gfx, S.t("Raiška"), null, scaleNames, scaleSel, idx -> prefs().edit().putInt("renderScale", scaleVals[idx]).apply());
        // HD graphics tier from the SC:R CASC. SD = classic 8-bit world (safe fallback).
        // HD2 = half-res HD (default). FULL = full HD (needs a strong device; degrades to
        // SD if its assets can't load).
        final String[] tierVals = {"sd", "hd2", "full"};
        int tierSel = 1;
        for (int i = 0; i < tierVals.length; ++i) if (tierVals[i].equals(prefs().getString("gfxTier", "hd2"))) tierSel = i;
        setSegments(gfx, S.t("Grafika (HD)"), S.t("HD2 rekomenduojama. FULL HD reikia SC:R ir galingos planšetės."),
                new String[]{"SD", "HD2", "FULL HD"}, tierSel, idx -> prefs().edit().putString("gfxTier", tierVals[idx]).apply());
        if (prefs().getString("scrCascDir", "").isEmpty())
            setNote(gfx, S.t("SC:R aplankas nepasirinktas — HD valdikliai veiks tik pasirinkus jį (Failai)."));
        setSwitch(gfx, S.t("Emissive švytėjimas"), "emissiveGlow",
                S.t("SC:R vienetų savišvytėjimas — ginklų blyksniai, psi, skydai, warp."), true);
        setSlider(gfx, S.t("Minkšti šešėliai"),
                S.t("Vienetų šešėlių švelnumas. 0 = klasikinis kietas juodas; didesnis — šviesesnis."), "shadowSoft", 0, 100);
        setSwitch(gfx, S.t("Kryptinis saulės šešėlis"), "sunShadow",
                S.t("Pastatai ir vienetai meta pakreiptą šešėlį."), false);
        setSlider(gfx, S.t("Šešėlio ilgis"),
                S.t("0 = beveik plokščias."), "sunShadowLen", 40, 100);
        setSwitch(gfx, S.t("Dinaminės šviesos"), "pointLights",
                S.t("Burtai, ugnis ir sprogimai apšviečia aplinką. Tik HD."), false);
        setSlider(gfx, S.t("Šviesų stiprumas"), null, "lightStrength", 100, 200);
        setSlider(gfx, S.t("Bloom (švytėjimo aureolė)"),
                S.t("0 = išjungta."), "bloomLevel", 0, 100);
        // GPU sharpen: default ON with AUTO effect -> the engine enables it on Adreno GPUs
        // and stays on the classic present elsewhere.
        setSwitch(gfx, S.t("Ryškumo filtras (Sharpen)"), "sharpenFilter",
                S.t("HD2 grafika atrodo aštriau."), true);
        setSlider(gfx, S.t("Sharpen stiprumas"),
                S.t("Per stipriai — triukšmas ir aureolės aplink kraštus."), "sharpenLevel", 35, 100);
        setSlider(gfx, S.t("Kinematinis atspalvis"),
                S.t("Kontrastas, sodrumas, šiluma. 0 = išjungta."), "gradeLevel", 0, 100);

        // ---- Valdymas ----
        LinearLayout ctl = settingsSection(S.t("Valdymas"));
        setSwitch(ctl, S.t("Rodyti SHIFT mygtuką"), "showQueue",
                S.t("Klasikiniame HUD: komandų eilės ir pažymėjimo papildymo mygtukas."), true);
        setSwitch(ctl, S.t("Bakstelėjimas = komanda"), "tapCommand",
                S.t("Bakstelėjimas žemėje ar į priešą duoda komandą pažymėtiems."), true);
        setSwitch(ctl, S.t("LAN sklandumas"), "renderInterp", S.t("Sklandesnis vienetų judėjimas LAN žaidime."), true);
        setSlider(ctl, S.t("Kameros greitis"), S.t("Krašto ir klaviatūros slinkimo greitis (100 = įprastas)."), "cameraSpeed", 100, 250);
        setSwitch(ctl, S.t("Rodyti zoom mygtukus"), "showZoomButtons", S.t("Ekrane rodomi - / + mygtukai. Pinch-zoom veikia ir be jų."), false);

        // ---- Žaidimo HUD: the classic bar (v2) or the touch HUD (v3) and its options ----
        LinearLayout hud = settingsSection(S.t("Žaidimo HUD"));
        setSegments(hud, S.t("HUD stilius"), S.t("Lietimo HUD: komandų ratas po pirštu, dokas kampe, be juostos apačioje."),
                new String[]{S.t("Klasikinis"), S.t("Lietimo")}, "v3".equals(prefs().getString("hudStyle", "v3")) ? 1 : 0,
                idx -> prefs().edit().putString("hudStyle", idx == 1 ? "v3" : "v2").apply());
        setNote(hud, S.t("Toliau — tik lietimo HUD."));
        setSegments(hud, S.t("Laikysena"), S.t("Rankose: lankinis dokas prie nykščio, RATAS fiksatorius, kameros svirtelė."),
                new String[]{S.t("Ant stalo / rašiklis"), S.t("Rankose")}, prefs().getInt("hud3Template", 0) == 1 ? 1 : 0,
                idx -> prefs().edit().putInt("hud3Template", idx).apply());
        setSegments(hud, S.t("Komandų dokas"), null,
                new String[]{S.t("Pagal laikyseną"), "3×3", S.t("Lankas")}, Math.max(0, Math.min(2, prefs().getInt("hud3Dock", 0))),
                idx -> prefs().edit().putInt("hud3Dock", idx).apply());
        setRangeSlider(hud, S.t("Rato delsa"), S.t("Kiek laikyti pirštą, kol atsidaro ratas."), "hud3WheelMs", 300, 200, 600, " ms");
        setSegments(hud, S.t("Rašiklio mygtukas"), S.t("Ratas: mygtukas + prisilietimas atidaro ratą iš karto."),
                new String[]{S.t("Ratas"), "Shift"}, "shift".equals(prefs().getString("hud3Pen", "wheel")) ? 1 : 0,
                idx -> prefs().edit().putString("hud3Pen", idx == 1 ? "shift" : "wheel").apply());
        setSwitch(hud, S.t("Fiksatorius ir svirtelė visur"), "hud3Latch",
                S.t("RATAS fiksatorius ir kameros svirtelė ir laikysenoje „Ant stalo“."), false);
        setSwitch(hud, S.t("Užrašai po doko ikonomis"), "hud3Labels", null, false);
        setSwitch(hud, S.t("Kairiarankis"), "hud3Left", S.t("Dokas kairėje, minimapas dešinėje."), false);
        setRangeSlider(hud, S.t("HUD mastelis"), null, "hud3Scale", 100, 80, 140, " %");
        setSwitch(hud, S.t("Vibracija"), "hud3Vibrate", null, true);
        setSwitch(hud, S.t("Rodyti visas 10 grupių"), "hud3AllGroups", S.t("Išjungus — tik užpildytos grupės."), false);

        // ---- Sąsaja ----
        LinearLayout ifc = settingsSection(S.t("Sąsaja"));
        final String[] splashModes = {"classic", "hd", "off"};
        setSegments(ifc, S.t("Paleidimo ekranas"), S.t("Žaidimo pradžios paveikslas iš jūsų failų. HD — iš SC:R failų."),
                new String[]{S.t("Klasikinis"), "HD", S.t("Išjungtas")},
                Math.max(0, Arrays.asList(splashModes).indexOf(prefs().getString("startSplash", "classic"))),
                idx -> prefs().edit().putString("startSplash", splashModes[idx]).apply());
        final String[] langCodes = {S.EN, S.LT};
        setSegments(ifc, S.t("Kalba"), null, new String[]{"English", "Lietuvių"}, S.LT.equals(prefs().getString("lang", S.EN)) ? 1 : 0, idx -> {
            String code = langCodes[idx];
            if (code.equals(prefs().getString("lang", S.EN))) return;
            prefs().edit().putString("lang", code).apply();
            S.setLanguage(code);
            restoreTab = TAB_NUSTATYMAI;
            recreate();
        });
        setSwitch(ifc, S.t("Rodyti resursų kiekį žemėlapyje"), "resourceCounts", S.t("Virš mineralų ir dujų rodomas likutis."), true);
        setSwitch(ifc, S.t("Rodyti saturaciją"), "showSaturation", S.t("Kiek darbininkų dirba prie mineralų klasterio ir prie dujų."), true);
        setSwitch(ifc, S.t("Rodyti FPS"), "showFps", S.t("Kadrai per sekundę viršutiniame kairiajame kampe."), true);
        setSwitch(ifc, S.t("Rodyti APM"), "showApm", S.t("Veiksmai per minutę, po laikrodžiu."), false);
        setSwitch(ifc, S.t("Visada rodyti sveikatos juostas"), "healthBarsAlways",
                S.t("Sveikatos juostos po VISAIS vienetais, ne tik pažeistais ar pažymėtais."), false);
        setSwitch(ifc, S.t("Equipment ikonos pilkos"), "equipGrey",
                S.t("Išjunk, jei naudoji spalvotų ikonų modą."), true);

        // ---- Žaidimas: default game speed (in-game +/- keys still override) ----
        LinearLayout game = settingsSection(S.t("Žaidimas"));
        final String[] speedNames = new String[SPEED_NAMES.length];
        int sIdx = 2;
        for (int k = 0; k < SPEED_NAMES.length; k++) {
            speedNames[k] = S.t(SPEED_NAMES[k]);
            if (SPEED_PCT[k] == prefs().getInt("gameSpeed", 100)) sIdx = k;
        }
        setSegments(game, S.t("Žaidimo greitis"), S.t("LAN žaidime galioja hosto greitis."), speedNames, sIdx,
                idx -> prefs().edit().putInt("gameSpeed", SPEED_PCT[idx]).apply());

        // ---- Modų talpykla: repository address + token ----
        LinearLayout repo = settingsSection(S.t("Modų talpykla"));
        LinearLayout addrRow = setRow(S.t("Talpyklos adresas…"), S.t("Lieka tik šiame įrenginyje."));
        addrRow.addView(kit.smallButton(S.t("Keisti"), this::configureRepo), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40)));
        repo.addView(addrRow);

        // ---- Dev: switches that bypass or instrument normal play ----
        LinearLayout dev = settingsSection(S.t("Dev"));
        setSwitch(dev, S.t("Atrakinti visas kampanijos misijas"), "campaignUnlockAll",
                S.t("Leidžia žaisti bet kurią misiją nebaigus ankstesnės."), false);
        setSwitch(dev, S.t("Išjungti karo rūką (Fog of War)"), "devNoFog",
                S.t("Visas žemėlapis matomas."), false);
        setSwitch(dev, S.t("Triggerių pėdsakas į logą"), "devTriggerTrace",
                S.t("Kampanijų diagnostikai: registracija ir sąlygų reikšmės į openbw-log.txt."), false);
        setSwitch(dev, S.t("AI skriptų pėdsakas į logą"), "devAiTrace",
                S.t("Praneša, kuriuos aiscript opcodes skriptai realiai pasiekia, bet variklis dar nevykdo."), false);

        // ---- Apie ----
        LinearLayout abt = settingsSection(S.t("Apie"));
        TextView title = kit.text("open1161HD  v" + versionName(), Color.WHITE, 18);
        title.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        title.setPadding(0, dp(4), 0, dp(4));
        abt.addView(title);
        setNote(abt, S.t("Geriausias kosmoso RTS.\n"
                  + "openBW+ (OpenBW-Android) atšaka.\n\n"
                  + "Variklis: OpenBW — atviro kodo RTS variklis.\n\n"
                  + "Naudoja jūsų paties žaidimo duomenų failus.") + "\n\n"
                + S.t("Įrenginys: ") + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL
                + "  (Android " + android.os.Build.VERSION.RELEASE + ")");
        // Bundled-bot attribution: Steamhammer and UAlbertaBot/BOSS are third-party MIT
        // and their copyright + licence notice must ship with any copy (the MIT term).
        setNote(abt, S.t("Įtaisyti botai:") + "\n"
                + "• UnifiedAI — open1161HD\n"
                + "• Steamhammer — © 2017 Hannes Bredberg (MIT)\n"
                + "• BOSS, UAlbertaBot — © 2016 David Churchill (MIT)\n\n"
                + "Steamhammer, BOSS and UAlbertaBot are used under the MIT License. "
                + "Permission notice: the above copyright notices and this permission "
                + "notice are included; the software is provided \"as is\", without warranty "
                + "of any kind.");
        LinearLayout licRow = setRow(S.t("Licencijos"), null);
        licRow.addView(kit.smallButton(S.t("Rodyti"), this::showLicenses), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40)));
        abt.addView(licRow);

        // ---- the sub-tab strip + one visible section; wide screens: two columns ----
        if (settingsTab < 0 || settingsTab >= setTabBoxes.size()) settingsTab = 0;
        final FrameLayout holder = new FrameLayout(this);
        final List<View> shown = new ArrayList<>();
        for (LinearLayout box : setTabBoxes) {
            View v = box;
            int n = box.getChildCount();
            if (wide && n >= 6) {
                // split the rows into two columns, keeping their order (left, then right)
                LinearLayout two = new LinearLayout(this);
                two.setOrientation(LinearLayout.HORIZONTAL);
                two.setBaselineAligned(false);
                LinearLayout left = new LinearLayout(this), right = new LinearLayout(this);
                left.setOrientation(LinearLayout.VERTICAL);
                right.setOrientation(LinearLayout.VERTICAL);
                int half = (n + 1) / 2;
                List<View> kids = new ArrayList<>();
                for (int i = 0; i != n; ++i) kids.add(box.getChildAt(i));
                box.removeAllViews();
                for (int i = 0; i != n; ++i) (i < half ? left : right).addView(kids.get(i));
                LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                l1.rightMargin = dp(28);
                two.addView(left, l1);
                two.addView(right, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
                v = two;
            }
            v.setVisibility(View.GONE);
            shown.add(v);
            holder.addView(v, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        shown.get(settingsTab).setVisibility(View.VISIBLE);
        LinearLayout strip = kit.segmented(setTabNames.toArray(new String[0]), settingsTab, idx -> {
            settingsTab = idx;
            for (int i = 0; i != shown.size(); ++i) shown.get(i).setVisibility(i == idx ? View.VISIBLE : View.GONE);
        });
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44));
        slp.bottomMargin = dp(10);
        page.addView(strip, slp);
        LinearLayout panel = spPanel();
        panel.setPadding(dp(16), dp(10), dp(16), dp(12));
        panel.addView(holder);
        page.addView(panel);
    }

    // Third-party licence texts shipped in assets/licenses/.
    private void showLicenses() {
        StringBuilder sb = new StringBuilder();
        try {
            String[] names = getAssets().list("licenses");
            if (names != null) {
                Arrays.sort(names);
                for (String n : names) {
                    sb.append("=== ").append(n).append(" ===\n\n");
                    try (InputStream in = getAssets().open("licenses/" + n)) {
                        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                        byte[] buf = new byte[8192];
                        int k;
                        while ((k = in.read(buf)) > 0) bo.write(buf, 0, k);
                        sb.append(bo.toString("UTF-8")).append("\n\n");
                    }
                }
            }
        } catch (Exception e) {
            sb.append(String.valueOf(e.getMessage()));
        }
        TextView t = kit.text(sb.toString(), Kit.TEXT, 12);
        t.setTextIsSelectable(true);
        t.setPadding(dp(18), dp(10), dp(18), dp(10));
        ScrollView sv = new ScrollView(this);
        sv.addView(t);
        new AlertDialog.Builder(this).setTitle(S.t("Licencijos")).setView(sv).setPositiveButton(S.t("Uždaryti"), null).show();
    }

    // Extract the APK-bundled bots (assets/bots/<abi>/lib<id>.so + assets/bots/
    // <id>.json) into files/bots/ so the launcher's bot picker finds them with no
    // download. These are part of the SIGNED APK -> trusted -> no executable-code
    // consent gate (unlike downloaded/imported bots). Runs once per app version
    // (bundledBotsVc guard); user-imported bots with other names are untouched.
    private void extractBundledBots() {
        try {
            int vc = versionCodeOf();
            if (prefs().getInt("bundledBotsVc", -1) == vc) return;
            String abi = null;
            for (String a : android.os.Build.SUPPORTED_ABIS) {
                String[] libs = getAssets().list("bots/" + a);
                if (libs != null && libs.length > 0) { abi = a; break; }
            }
            if (abi == null) return; // no bundle for this device's ABI
            File botsDir = new File(getFilesDir(), "bots");
            botsDir.mkdirs();
            // Bots that were once bundled but are gone from the APK (the Lite family
            // and the demo -- all absorbed by the built-in unified AI): remove their
            // old copies so they don't linger in files/bots. Only these hardcoded
            // names; anything the user imported is never touched.
            for (String old : new String[]{ "libsteamlite.so", "steamlite.json", "libaiurlite.so", "aiurlite.json",
                                            "libzerglite.so", "zerglite.json", "libunifiedlite.so", "unifiedlite.json",
                                            "libbwapidemo.so", "bwapidemo.json" })
                new File(botsDir, old).delete();
            for (String so : getAssets().list("bots/" + abi))
                copyAsset("bots/" + abi + "/" + so, new File(botsDir, so));
            for (String f : getAssets().list("bots"))        // arch-independent manifests
                if (f.endsWith(".json")) copyAsset("bots/" + f, new File(botsDir, f));
            prefs().edit().putInt("bundledBotsVc", vc).apply();
        } catch (Exception e) {
            android.util.Log.w("openbw-bots", "extractBundledBots: " + e);
        }
    }

    private void copyAsset(String assetPath, File out) throws Exception {
        try (InputStream in = getAssets().open(assetPath);
             java.io.FileOutputStream fo = new java.io.FileOutputStream(out)) {
            byte[] b = new byte[65536]; int n;
            while ((n = in.read(b)) > 0) fo.write(b, 0, n);
        }
        out.setExecutable(true, false); // dlopen needs +x (no-op cost for .json)
    }

    private void pickFiles(int requestCode) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(intent, requestCode);
    }

    // open1161hd: optional SC:R (Remastered) CASC asset source. Unlike the BW
    // 1.16.1 MPQ path (which imports a few named .mpq files), SC:R data is a
    // large read-only directory tree (data/data/*.idx + data.### + .build.info),
    // so it is read IN PLACE rather than copied. The native CASC reader uses
    // fopen on a real filesystem path, which on Android 11+ needs all-files
    // access -- a SAF content:// URI is not fopen-able. This section only exists
    // on the open1161hd (HD) branch; main's launcher stays MPQ-only.
    // Faze 6: dot + status line for the SC:R folder. Green "SC:R OK (HD)" when a
    // validated folder is set (CASC is then on automatically), red "No SC:R folder
    // (SD/HD2 only)" when not. Replaces the old full-path line + CASC checkbox.
    private void updateScrIndicator() {
        boolean set = !prefs().getString("scrCascDir", "").isEmpty();
        if (scrIndicator != null) {
            GradientDrawable dot = new GradientDrawable();
            dot.setShape(GradientDrawable.OVAL);
            dot.setColor(set ? Color.rgb(80, 200, 100) : Color.rgb(200, 80, 80));
            scrIndicator.setBackground(dot);
        }
        if (scrStatus != null) {
            scrStatus.setText(set ? S.t("SC:R rasta — HD įjungta.")
                                  : S.t("SC:R aplankas nepasirinktas — tik SD / HD2."));
            scrStatus.setTextColor(set ? Color.rgb(120, 210, 140) : Color.rgb(220, 120, 120));
        }
    }

    private void pickScrFolder() {
        // All-files access is required so the native CASC reader can fopen the
        // real path. Ask for it first; the user grants it once, then re-taps.
        if (android.os.Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            new AlertDialog.Builder(this)
                    .setTitle(S.t("Reikia „Visų failų prieigos“"))
                    .setMessage(S.t("Suteik „Visų failų prieigą“ ir vėl paspausk „Pasirinkti SC:R aplanką“."))
                    .setPositiveButton(S.t("Atidaryti nustatymus"), (d, w) -> {
                        try {
                            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                    Uri.parse("package:" + getPackageName())));
                        } catch (Exception e) {
                            try { startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
                            catch (Exception ignored) {}
                        }
                    })
                    .setNegativeButton(S.t("Atšaukti"), null)
                    .show();
            return;
        }
        try {
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_PICK_SCR);
        } catch (Exception e) {
            Toast.makeText(this, S.t("Nepavyko atidaryti aplankų parinkiklio"), Toast.LENGTH_SHORT).show();
        }
    }

    // Resolve a SAF tree URI (primary:Foo/Bar or <sd>:Foo) to a real filesystem
    // path the native CASC reader can fopen. Only meaningful with all-files
    // access; the picker is just a convenient folder chooser here.
    private String treeUriToPath(Uri treeUri) {
        try {
            String docId = DocumentsContract.getTreeDocumentId(treeUri);
            String[] parts = docId.split(":", 2);
            String vol = parts[0];
            String rel = parts.length > 1 ? parts[1] : "";
            if ("primary".equalsIgnoreCase(vol)) {
                return new File(Environment.getExternalStorageDirectory(), rel).getAbsolutePath();
            }
            return "/storage/" + vol + (rel.isEmpty() ? "" : "/" + rel);
        } catch (Exception e) {
            return null;
        }
    }

    private void onScrFolderPicked(Uri treeUri) {
        String path = treeUriToPath(treeUri);
        if (path == null) {
            Toast.makeText(this, S.t("Nepavyko nustatyti aplanko kelio"), Toast.LENGTH_LONG).show();
            return;
        }
        File root = new File(path);
        boolean ok = new File(root, ".build.info").exists() && new File(root, "data/data").isDirectory();
        if (!ok) {
            new AlertDialog.Builder(this)
                    .setTitle(S.t("Ne SC:R šaknis?"))
                    .setMessage(S.t("Tai ne SC:R įdiegimo aplankas. Pasirink aplanką, kuriame yra „data“ ir „.build.info“.") + "\n\n" + path)
                    .setPositiveButton(S.t("Gerai"), null)
                    .show();
            return;
        }
        // Faze 6: choosing a valid folder auto-enables CASC (no separate toggle).
        prefs().edit().putString("scrCascDir", path).apply();
        updateScrIndicator();
        Toast.makeText(this, S.t("SC:R aplankas nustatytas"), Toast.LENGTH_SHORT).show();
        if (gateMode) setContentView(buildSetupGate());
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_SCR) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) onScrFolderPicked(data.getData());
            return;
        }
        if (resultCode != RESULT_OK || data == null) return;
        List<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) {
            for (int i = 0; i < data.getClipData().getItemCount(); ++i) {
                uris.add(data.getClipData().getItemAt(i).getUri());
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        if (uris.isEmpty()) return;
        importInBackground(uris, requestCode);
    }

    private void importInBackground(List<Uri> uris, int requestCode) {
        final boolean isMpq = requestCode == REQ_PICK_MPQ;
        final boolean isMap = requestCode == REQ_PICK_MAP;
        final boolean isBot = requestCode == REQ_PICK_BOT;
        final boolean isMod = requestCode == REQ_PICK_MOD;
        final TextView progressText = new TextView(this);
        int pad = dp(24);
        progressText.setPadding(pad, pad, pad, pad);
        progressText.setTextColor(Color.WHITE);
        progressText.setText(S.t("Ruošiamasi…"));
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(isMpq ? "Importuojami MPQ failai"
                        : isMap ? "Importuojami žemėlapiai"
                        : isBot ? "Importuojami botai"
                        : isMod ? "Importuojami modai" : "Importuojami replay failai")
                .setView(progressText)
                .setCancelable(false)
                .create();
        dialog.show();

        new Thread(() -> {
            int copied = 0;
            List<String> warnings = new ArrayList<>();
            File filesDir = getExternalFilesDir(null);
            for (Uri uri : uris) {
                String name = displayName(uri);
                if (name == null) continue;
                final String shownName = name;
                ui.post(() -> progressText.setText(S.t("Kopijuojama:\n") + shownName));
                try {
                    File dst;
                    boolean mpq;
                    String lower = name.toLowerCase(Locale.ROOT);
                    if (isMod) {
                        // v0.26: mod package = zip of the mod dir's CONTENT;
                        // unpacks to files/mods/<zip-base>/ (bots/*.so entries
                        // go to internal storage - external is noexec).
                        if (!lower.endsWith(".zip")) continue;
                        String id = name.substring(0, name.length() - 4);
                        if (id.toLowerCase(Locale.ROOT).endsWith(".obwmod")) id = id.substring(0, id.length() - 7);
                        id = id.replaceAll("[^A-Za-z0-9._-]", "_");
                        if (id.isEmpty()) continue;
                        // v0.83.008 (S1.1): look inside before unpacking. A mod
                        // that ships .so files ships EXECUTABLE CODE, and that
                        // deserves its own yes/no -- installing a graphics pack
                        // and running someone's binary are not the same act.
                        ZipScan scan = scanModZip(uri);
                        boolean allowNative = false;
                        if (!scan.nativeLibs.isEmpty()) {
                            allowNative = askNativeConsent(name, scan.nativeLibs);
                            if (!allowNative) {
                                warnings.add(name + ": " + S.t("botų bibliotekos praleistos (nepatvirtinta)"));
                            }
                        }
                        int files = unzipMod(uri, new File(new File(filesDir, "mods"), id), allowNative);
                        if (files > 0) ++copied;
                        else warnings.add(name + ": tuščias arba netinkamas zip");
                        continue;
                    }
                    if (isMpq) {
                        String canonical = canonicalMpqName(name);
                        if (canonical == null && !lower.endsWith(".mpq")) continue;
                        dst = new File(filesDir, canonical != null ? canonical : name);
                        mpq = true;
                    } else if (isMap) {
                        if (!lower.endsWith(".scm") && !lower.endsWith(".scx")) continue;
                        File dir = new File(filesDir, "maps");
                        dir.mkdirs();
                        dst = new File(dir, name);
                        mpq = false;
                    } else if (isBot) {
                        // Bot libraries must live in INTERNAL storage:
                        // external is mounted noexec, dlopen would fail.
                        if (!lower.endsWith(".so")) continue;
                        File dir = new File(getFilesDir(), "bots");
                        dir.mkdirs();
                        dst = new File(dir, name);
                        mpq = false;
                    } else {
                        if (!lower.endsWith(".rep")) continue;
                        File dir = new File(filesDir, "replays");
                        dir.mkdirs();
                        dst = new File(dir, name);
                        mpq = false;
                    }
                    copyUri(uri, dst);
                    if (mpq) {
                        String w = validateMpq(dst);
                        if (w != null) warnings.add(w);
                    }
                    ++copied;
                } catch (Exception e) {
                    warnings.add("Nepavyko nukopijuoti " + name + ": " + e.getMessage());
                }
            }
            final int copiedFinal = copied;
            ui.post(() -> {
                dialog.dismiss();
                if (!warnings.isEmpty()) {
                    new AlertDialog.Builder(this)
                            .setTitle(S.t("Importuota su pastabomis"))
                            .setMessage(S.t("Nukopijuota failų: ") + copiedFinal + "\n\n"
                                    + android.text.TextUtils.join("\n\n", warnings))
                            .setPositiveButton(S.t("Gerai"), null)
                            .show();
                } else {
                    Toast.makeText(this, S.t("Nukopijuota failų: ") + copiedFinal, Toast.LENGTH_SHORT).show();
                }
                refresh();
            });
        }, "import").start();
    }

    /**
     * v0.26: unpack a mod .zip (content of the mod dir at the zip root) into
     * files/mods/&lt;id&gt;/. bots/*.so entries go to internal bots/ (external is
     * noexec). Zip-slip guarded. Returns the number of files written.
     */
    /**
     * Ask, on the UI thread, whether the archive's native libraries may be
     * installed, and block the import thread until the user answers. Blocking
     * is deliberate: the alternative is unpacking first and asking later, which
     * is not consent.
     */
    private boolean askNativeConsent(String zipName, List<String> libs) throws InterruptedException {
        final java.util.concurrent.CountDownLatch latch = new java.util.concurrent.CountDownLatch(1);
        final boolean[] answer = {false};
        StringBuilder list = new StringBuilder();
        for (int i = 0; i < libs.size() && i < 8; ++i) list.append("\n• ").append(libs.get(i));
        if (libs.size() > 8) list.append("\n• … (+").append(libs.size() - 8).append(")");
        final String msg = S.t("Šiame mode yra vykdomo kodo (botų bibliotekų):") + list
                + "\n\n" + S.t("Jos veiks su visomis šios programos teisėmis. "
                + "Diegti jas tik jei pasitiki šaltiniu.");
        runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle(S.t("Įdiegti vykdomą kodą?") + " — " + zipName)
                .setMessage(msg)
                .setCancelable(false)
                .setPositiveButton(S.t("Įdiegti"), (d, w) -> { answer[0] = true; latch.countDown(); })
                .setNegativeButton(S.t("Praleisti"), (d, w) -> { answer[0] = false; latch.countDown(); })
                .show());
        latch.await();
        return answer[0];
    }

    /** What a mod zip contains, read before a single byte is written. */
    static final class ZipScan {
        int entries;
        long declaredBytes;
        final List<String> nativeLibs = new ArrayList<>();
    }

    // A mod is a few hundred MB of art (hd2 measured at 654 MB unpacked), so
    // the ceilings are generous -- they exist to stop a malformed or hostile
    // archive from filling the device, not to police normal mods.
    private static final int MOD_MAX_ENTRIES = 100000;
    private static final long MOD_MAX_BYTES = 4L * 1024 * 1024 * 1024;

    /**
     * v0.83.008 (S1.1): read the archive's table of contents first. The point
     * is the .so list: those are EXECUTABLE CODE, and the user gets to say yes
     * to them separately from "install this mod".
     */
    private ZipScan scanModZip(Uri uri) throws Exception {
        ZipScan s = new ZipScan();
        try (java.util.zip.ZipInputStream zin =
                new java.util.zip.ZipInputStream(getContentResolver().openInputStream(uri))) {
            java.util.zip.ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                ++s.entries;
                if (e.getSize() > 0) s.declaredBytes += e.getSize();
                String en = e.getName().replace('\\', '/');
                if (en.toLowerCase(Locale.ROOT).endsWith(".so")) s.nativeLibs.add(en);
            }
        }
        return s;
    }

    /**
     * v0.26: unpack a mod .zip (content of the mod dir at the zip root) into
     * files/mods/&lt;id&gt;/. bots/*.so entries go to internal bots/ (external is
     * noexec). Returns the number of files written.
     *
     * v0.83.008 (S1.1) hardening. The zip-slip guard was already here and works.
     * What was actually weak, and is fixed here:
     *   - the prefix test had no separator, so a sibling directory whose name
     *     merely STARTS with the mod's name would pass ("mods/hd2evil" against
     *     base "mods/hd2");
     *   - parent directories were created BEFORE the check, so a rejected entry
     *     still left its directories behind;
     *   - there was no ceiling on entry count or total bytes;
     *   - and native code was unpacked with no separate consent. `allowNative`
     *     now carries the user's answer; false skips those entries and installs
     *     the rest of the mod.
     */
    private int unzipMod(Uri uri, File modDir, boolean allowNative) throws Exception {
        int files = 0;
        long written = 0;
        int seen = 0;
        modDir.mkdirs();
        String modBase = modDir.getCanonicalPath();
        File botsDir = new File(getFilesDir(), "bots");
        String botsBase = botsDir.getCanonicalPath();
        java.util.List<String> deviceAbis = java.util.Arrays.asList(android.os.Build.SUPPORTED_ABIS);
        try (java.util.zip.ZipInputStream zin =
                new java.util.zip.ZipInputStream(getContentResolver().openInputStream(uri))) {
            java.util.zip.ZipEntry e;
            byte[] buf = new byte[65536];
            while ((e = zin.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                if (++seen > MOD_MAX_ENTRIES) break;
                String en = e.getName().replace('\\', '/');
                if (en.contains("..") || en.startsWith("/")) continue;
                boolean isNative = en.toLowerCase(Locale.ROOT).endsWith(".so");
                if (isNative && !allowNative) continue;
                File out;
                String base;
                // A bot's mod.json manifest (bots/<id>.json, schema openbw-bot/1)
                // rides next to its .so so the picker can show name/race without
                // dlopening code. It is data, not native code -- no consent gate.
                boolean botManifest = !isNative && en.startsWith("bots/")
                        && en.toLowerCase(Locale.ROOT).endsWith(".json");
                boolean botNative = isNative && en.startsWith("bots/");
                if (botNative) {
                    // Arch-aware: a multi-arch bot mod ships bots/<abi>/lib<id>.so
                    // for each ABI -- extract ONLY the entry matching this device
                    // (else same-basename arches would collide in files/bots/).
                    // A flat bots/lib<id>.so (single-arch) always extracts.
                    String rel = en.substring(5); // after "bots/"
                    int slash = rel.indexOf('/');
                    if (slash > 0 && !deviceAbis.contains(rel.substring(0, slash))) continue;
                    botsDir.mkdirs();
                    out = new File(botsDir, new File(en).getName());
                    base = botsBase;
                } else if (botManifest) {
                    botsDir.mkdirs();
                    out = new File(botsDir, new File(en).getName());
                    base = botsBase;
                } else if (isNative) {
                    continue;   // native code only from bots/, never loose
                } else {
                    out = new File(modDir, en);
                    base = modBase;
                }
                // Canonical containment, WITH the separator, before anything is
                // created on disk.
                String path = out.getCanonicalPath();
                if (!path.equals(base) && !path.startsWith(base + File.separator)) continue;
                File parent = out.getParentFile();
                if (parent != null) parent.mkdirs();
                try (java.io.FileOutputStream fo = new java.io.FileOutputStream(out)) {
                    int n;
                    while ((n = zin.read(buf)) > 0) {
                        written += n;
                        if (written > MOD_MAX_BYTES) throw new java.io.IOException(
                                "mod exceeds " + (MOD_MAX_BYTES >> 30) + " GB unpacked");
                        fo.write(buf, 0, n);
                    }
                }
                ++files;
            }
        }
        return files;
    }

    /**
     * v0.83.010 (S1.5): one place that opens a repository URL.
     *
     * Two things it does that a plain openConnection() does not:
     *
     *  - sends the user's token when there is one. The token lives in this
     *    device's prefs and NOWHERE in the APK: a key compiled into the app is
     *    a published key, since an APK is unpacked in a minute.
     *  - follows redirects by hand so it can DROP the Authorization header when
     *    the redirect crosses to another host. GitHub release assets redirect
     *    to pre-signed S3 URLs that reject a request carrying both their
     *    signature and someone else's Authorization header -- and sending a
     *    token to a host the user never named would be wrong anyway.
     */
    private java.net.HttpURLConnection openRepoConn(String url, String accept) throws Exception {
        String token = prefs().getString("modRepoToken", "").trim();
        String current = url;
        java.net.HttpURLConnection c = null;
        String firstHost = new java.net.URL(url).getHost();
        for (int hop = 0; hop < 5; ++hop) {
            java.net.URL u = new java.net.URL(current);
            c = (java.net.HttpURLConnection) u.openConnection();
            c.setInstanceFollowRedirects(false);
            c.setConnectTimeout(10000);
            c.setReadTimeout(30000);
            if (accept != null) c.setRequestProperty("Accept", accept);
            // GitHub's API rejects requests without a User-Agent. Dalvik sends
            // one, but naming ourselves makes a rate-limit or block traceable.
            c.setRequestProperty("User-Agent", "open1161HD/" + versionCodeOf());
            if (!token.isEmpty() && u.getHost().equals(firstHost)) {
                c.setRequestProperty("Authorization", "Bearer " + token);
            }
            int code = c.getResponseCode();
            if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                String loc = c.getHeaderField("Location");
                c.disconnect();
                if (loc == null) throw new java.io.IOException("redirect without Location");
                current = new java.net.URL(u, loc).toString();
                continue;
            }
            if (code == 401) {
                // Bad credentials: the token itself is wrong or expired.
                throw new java.io.IOException("401 -- " + (token.isEmpty()
                        ? "privatiems modams reikia prieigos kodo"
                        : "prieigos kodas netinka"));
            }
            if (code == 403 || code == 429) {
                // v0.83.036 (D12): a 403 with a VALID token is not "token
                // rejected" -- it is almost always a permission gap. Measured
                // against the real private repo: a fine-grained PAT with only
                // Metadata:Read authenticates and lists the repo, but every
                // Contents/release read returns 403. Telling the user "raktas
                // atmestas" sent them to regenerate the same wrong-scope token;
                // now it names the missing permission. Rate-limit (remaining 0)
                // is the other 403 cause, distinguished by the header.
                String remaining = c.getHeaderField("X-RateLimit-Remaining");
                c.disconnect();
                if ("0".equals(remaining)) {
                    throw new java.io.IOException(code + " -- GitHub užklausų limitas viršytas, pabandykite vėliau");
                }
                throw new java.io.IOException(code + " -- " + (token.isEmpty()
                        ? "privatiems modams reikia prieigos kodo"
                        : "prieigos kodas netinka"));
            }
            if (code >= 400) throw new java.io.IOException("HTTP " + code);
            return c;
        }
        throw new java.io.IOException("too many redirects");
    }

    private String httpGetText(String url) throws Exception {
        java.net.HttpURLConnection c = openRepoConn(url, "application/vnd.github.raw, application/json, */*");
        try (InputStream in = c.getInputStream()) {
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            return bo.toString("UTF-8");
        }
    }

    private String validateMpq(File f) {
        int idx = -1;
        for (int i = 0; i < MPQ_NAMES.length; ++i) {
            if (MPQ_NAMES[i].equalsIgnoreCase(f.getName())) { idx = i; break; }
        }
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] head = new byte[4];
            if (in.read(head) < 4) return f.getName() + ": failas per mažas, tai ne MPQ archyvas.";
            int magic = (head[0] & 0xff) | ((head[1] & 0xff) << 8) | ((head[2] & 0xff) << 16) | ((head[3] & 0xff) << 24);
            if (magic != MPQ_MAGIC) {
                return f.getName() + ": tai ne MPQ archyvas (blogas antraštės parašas). "
                        + "OpenBW reikia originalių StarCraft 1.16.1 MPQ failų, ne Remastered (CASC) versijos.";
            }
        } catch (Exception e) {
            return f.getName() + ": nepavyko perskaityti (" + e.getMessage() + ")";
        }
        if (idx >= 0) {
            long len = f.length();
            if (len < MPQ_MIN[idx] || len > MPQ_MAX[idx]) {
                return f.getName() + ": neįprastas dydis (" + (len / 1_000_000) + " MB). Tikėtasi ~"
                        + (MPQ_MIN[idx] / 1_000_000) + "–" + (MPQ_MAX[idx] / 1_000_000)
                        + " MB. Įsitikinkite, kad tai StarCraft 1.16.1 (ne Remastered) failas.";
            }
        }
        return null;
    }

    // Optional CD archives: the in-game music lives in these (not in
    // StarDat/BrooDat). The native side looks them up by exact name.
    private static final String[] OPTIONAL_MPQ_NAMES = {"StarCraft.mpq", "BroodWar.mpq"};

    private String canonicalMpqName(String name) {
        for (String m : MPQ_NAMES) {
            if (m.equalsIgnoreCase(name)) return m;
        }
        for (String m : OPTIONAL_MPQ_NAMES) {
            if (m.equalsIgnoreCase(name)) return m;
        }
        return null;
    }

    private List<String> mpqMissing() {
        List<String> missing = new ArrayList<>();
        for (String m : MPQ_NAMES) {
            if (!new File(getExternalFilesDir(null), m).exists()) missing.add(m);
        }
        return missing;
    }

    private void refresh() {
        List<String> missing = mpqMissing();
        boolean ready = missing.isEmpty();
        // While the first-run gate owns the window, none of the launcher views
        // below exist. Only recreate once the gate's condition is actually
        // satisfied -- recreating unconditionally puts onResume -> refresh ->
        // recreate into an infinite relaunch loop, which is exactly what a
        // first version of this did.
        if (gateMode) {
            // Rebuilt in place (import finished, SC:R picked, back from the access
            // settings); it leaves only through its own Continue button.
            setContentView(buildSetupGate());
            return;
        }

        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(ready ? Color.rgb(80, 200, 100) : Color.rgb(220, 70, 70));
        mpqIndicator.setBackground(dot);
        if (ready) {
            boolean music = new File(getExternalFilesDir(null), "StarCraft.mpq").exists()
                    || new File(getExternalFilesDir(null), "BroodWar.mpq").exists();
            mpqStatus.setText(music ? S.t("MPQ failai rasti.")
                    : S.t("MPQ failai rasti. Muzikai papildomai importuokite StarCraft.mpq ir BroodWar.mpq (CD archyvai)."));
            mpqStatus.setTextColor(Color.rgb(140, 220, 140));
        } else {
            mpqStatus.setText(S.t("Trūksta MPQ: ") + String.join(", ", missing));
            mpqStatus.setTextColor(Color.rgb(240, 180, 120));
        }

        if (mapList != null) populateMapList(mapList);
        if (mapListMp != null) populateMapList(mapListMp);

        renderReplayList();
        renderSaveList();

        // Keep selection valid across refreshes.
        if (selectedMap != null && !selectedMap.exists()) selectedMap = null;
        if (selectedReplay != null && !selectedReplay.exists()) selectedReplay = null;
    }

    // ---- First-run gate (v0.70 item 17) --------------------------------
    // Shown instead of the launcher until the three required archives are
    // present. importInBackground() ends with refresh(); the gate re-checks on
    // resume via recreate(), so completing the import drops straight into the
    // launcher.
    private boolean gateMode = false;
    private int gatePreview = 0;          // adb test hook, see onCreate

    private View buildSetupGate() {
        if (kit == null) kit = new Kit(this);
        final File gameDir = getExternalFilesDir(null);
        final boolean blank = gatePreview == 2;      // test hook: pretend nothing was picked yet
        java.util.List<String> missing = mpqMissing();
        final boolean bwOk = !blank && missing.isEmpty();
        String scrDir = blank ? "" : prefs().getString("scrCascDir", "");
        boolean scrContent = !scrDir.isEmpty() && new File(scrDir, ".build.info").exists() && new File(scrDir, "data/data").isDirectory();
        boolean access = !blank && (android.os.Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager());
        boolean scrOk = scrContent && access;
        final int GOOD = Kit.LABEL, BAD = Kit.DANGER, NONE = Kit.TEXT2;
        boolean wideCards = getResources().getConfiguration().screenWidthDp >= 700;

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(kit.box(Kit.PANEL, Kit.LINE));
        panel.setPadding(dp(18), dp(14), dp(18), dp(16));
        TextView title = kit.text(S.t("Pasirink savo žaidimo failus"), Color.WHITE, 22);
        title.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        panel.addView(title);
        TextView sub = kit.text(S.t("Programa neturi ir nesiunčia jokių žaidimo failų — juos nurodai pats iš savo legalios kopijos."), Kit.TEXT2, 13);
        sub.setPadding(0, dp(2), 0, dp(12));
        panel.addView(sub);

        LinearLayout cards = new LinearLayout(this);
        cards.setOrientation(wideCards ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);

        // --- StarCraft: Brood War 1.16.1 (required) ---
        LinearLayout bw = gateCard(bwOk ? GOOD : Kit.STEEL);
        bw.addView(gateCardHead("StarCraft: Brood War 1.16.1", S.t("būtina"), Kit.ACCENT,
                bwOk ? "\u2713 " + S.t("patikrinta") : S.t("nepasirinkta"), bwOk ? GOOD : Color.WHITE));
        final String[] names = {"StarDat.mpq", "BrooDat.mpq", "Patch_rt.mpq", "StarCraft.mpq", "BroodWar.mpq"};
        final String[] what = {"pagrindiniai duomenys, portretai, meniu garsai", "Brood War unitai ir portretai", "1.16.1 pataisos",
                "nebūtina · kampanijos, žemėlapiai, muzika", "nebūtina · BW kampanijos, muzika"};
        for (int i = 0; i != names.length; ++i) {
            File f = new File(gameDir, names[i]);
            boolean have = !blank && f.isFile();
            boolean required = i < 3;
            bw.addView(gateFileRow(names[i], S.t(what[i]),
                    have ? "\u2713" : required ? "\u2717 " + S.t("trūksta") : "\u2014 " + S.t("nėra"),
                    have ? GOOD : required ? BAD : NONE));
        }
        bw.addView(new android.widget.Space(this), new LinearLayout.LayoutParams(1, 0, 1f));
        LinearLayout.LayoutParams bwBtn = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        bwBtn.topMargin = dp(12);
        bw.addView(kit.button(S.t("Pasirinkti MPQ failus"), 0, () -> pickFiles(REQ_PICK_MPQ)), bwBtn);

        // --- StarCraft: Remastered (optional) ---
        LinearLayout sc = gateCard(scrOk ? GOOD : (!scrDir.isEmpty() && !scrOk) ? BAD : Kit.STEEL);
        sc.addView(gateCardHead("StarCraft: Remastered", S.t("nebūtina"), Color.WHITE,
                scrOk ? "\u2713 " + S.t("patikrinta · HD įjungta") : S.t("nepasirinkta"), scrOk ? GOOD : Color.WHITE));
        sc.addView(gateFileRow(S.t("Aplankas"), scrDir.isEmpty() ? S.t("SC:R įdiegimo aplankas") : scrDir,
                scrDir.isEmpty() ? "\u2014 " + S.t("nepasirinktas") : "\u2713", scrDir.isEmpty() ? NONE : GOOD));
        sc.addView(gateFileRow(S.t("Prieiga"), S.t("„Visų failų prieiga“"),
                access ? "\u2713 " + S.t("suteikta") : "\u2014 " + S.t("dar neprašyta"), access ? GOOD : NONE));
        TextView scNote = kit.text(S.t("Atrakina HD grafiką. Galima pridėti vėliau (Nustatymai ▸ Failai)."), Kit.TEXT2, 11);
        scNote.setPadding(0, dp(8), 0, dp(10));
        sc.addView(scNote);
        sc.addView(new android.widget.Space(this), new LinearLayout.LayoutParams(1, 0, 1f));
        sc.addView(kit.button(S.t("Pasirinkti SC:R aplanką"), 0, this::pickScrFolder),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        if (wideCards) {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
            l1.rightMargin = dp(12);
            cards.addView(bw, l1);
            cards.addView(sc, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
        } else {
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            l1.bottomMargin = dp(12);
            cards.addView(bw, l1);
            cards.addView(sc, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        panel.addView(cards);

        // --- footer: the portrait tile (our own static until there are files) + actions ---
        LinearLayout foot = new LinearLayout(this);
        foot.setOrientation(LinearLayout.HORIZONTAL);
        foot.setGravity(Gravity.CENTER_VERTICAL);
        foot.setPadding(0, dp(12), 0, 0);
        PortraitView tile = new PortraitView(this);
        tile.setScale(rowPortraitScale());
        tile.setBorderColor(bwOk ? GOOD : Kit.STEEL);
        tile.setPortrait(gameDir, bwOk ? PortraitStore.activeId(prefs(), activeProfile(), gameDir) : -1);
        foot.addView(tile);
        // (no caption next to the tile -- it explains nothing the player needs)
        foot.addView(new android.widget.Space(this), new LinearLayout.LayoutParams(0, 1, 1f));
        LinearLayout.LayoutParams exLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(52));
        exLp.rightMargin = dp(12);
        foot.addView(kit.button(S.t("Išeiti"), 0, this::finish), exLp);
        TextView go = kit.button(S.t("Tęsti"), 1, () -> {
            if (!mpqMissing().isEmpty()) return;
            prefs().edit().putBoolean("firstRunPending", false).apply();
            gateMode = false;
            getIntent().removeExtra("gatePreview");
            recreate();
        });
        go.setMinWidth(dp(140));
        go.setMinimumWidth(dp(140));
        if (!bwOk) { go.setAlpha(0.35f); go.setClickable(false); }
        foot.addView(go, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(52)));
        panel.addView(foot);

        // a modal over a dark, empty launcher: nothing behind it is reachable
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF05080A);
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        FrameLayout holder = new FrameLayout(this);
        int screenW = getResources().getDisplayMetrics().widthPixels;
        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(Math.min(dp(900), screenW - dp(24)),
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        plp.topMargin = dp(12);
        plp.bottomMargin = dp(12);
        holder.addView(panel, plp);
        sv.addView(holder, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(sv);
        return root;
    }

    private LinearLayout gateCard(int stroke) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(kit.box(0xFF070B0E, stroke));
        c.setPadding(dp(12), dp(10), dp(12), dp(12));
        return c;
    }

    private View gateCardHead(String title, String tag1, int tag1Color, String tag2, int tag2Color) {
        LinearLayout h = new LinearLayout(this);
        h.setOrientation(LinearLayout.HORIZONTAL);
        h.setGravity(Gravity.CENTER_VERTICAL);
        h.setPadding(0, 0, 0, dp(6));
        TextView t = kit.text(title, Color.WHITE, 16);
        t.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        h.addView(t);
        for (int i = 0; i != 2; ++i) {
            TextView g = kit.text(i == 0 ? tag1 : tag2, i == 0 ? tag1Color : tag2Color, 10);
            g.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
            g.setBackground(kit.box(0x00000000, i == 0 ? tag1Color : tag2Color));
            g.setPadding(dp(6), dp(1), dp(6), dp(1));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.leftMargin = dp(8);
            h.addView(g, lp);
        }
        return h;
    }

    private View gateFileRow(String name, String what, String status, int statusColor) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(7), 0, dp(7));
        TextView n = kit.text(name, Color.WHITE, 13);
        n.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        row.addView(n, new LinearLayout.LayoutParams(dp(104), ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView w = kit.text(what, Kit.TEXT2, 12);
        row.addView(w, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView st = kit.text(status, statusColor, 12);
        st.setPadding(dp(8), 0, 0, 0);
        row.addView(st);
        col.addView(row);
        View line = new View(this);
        line.setBackgroundColor(Kit.TRACK);
        col.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
        return col;
    }

    // ---- Score screen (v0.69 item 14) ----------------------------------
    // The original picks its background art by race AND outcome (glue\scoreTv,
    // scoreTd, scorePv, ... - six sets) and groups the table into Units,
    // Structures and Resources. We keep that information structure and the
    // race accent, but draw it in the launcher's own visual language rather
    // than extracting the 640x480 art (variant V2).
    private static final int[] RACE_COLOR = {0xFF9A6BC8, 0xFF4E8BD6, 0xFFD6B24E};

    private boolean showScoreScreenIfPending() {
        File f = new File(getExternalFilesDir(null), "score.txt");
        if (!f.isFile()) return false;
        java.util.List<String[]> rows = new java.util.ArrayList<>();
        boolean won = false;
        int local = -1, frames = 0;
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) {
                java.util.Map<String, String> kv = new java.util.HashMap<>();
                // name= is the last field and runs to the end of the line (may hold spaces)
                int nmAt = line.indexOf(" name=");
                if (nmAt >= 0) { kv.put("name", line.substring(nmAt + 6).trim()); line = line.substring(0, nmAt); }
                for (String tok : line.trim().split("\\s+")) {
                    int eq = tok.indexOf('=');
                    if (eq > 0) kv.put(tok.substring(0, eq), tok.substring(eq + 1));
                }
                if (kv.containsKey("won")) won = "1".equals(kv.get("won"));
                if (kv.containsKey("local")) local = parseIntSafe(kv.get("local"), -1);
                if (kv.containsKey("frames")) frames = parseIntSafe(kv.get("frames"), 0);
                if (kv.containsKey("p")) rows.add(new String[]{
                        kv.get("p"), kv.get("race"), kv.get("units"), kv.get("buildings"),
                        kv.get("kills"), kv.get("losses"), kv.get("killscore"),
                        kv.get("razescore"), kv.get("minerals"), kv.get("gas"),
                        kv.get("color"), kv.get("portrait"), kv.get("name")});
            }
        } catch (Exception e) {
            f.delete();
            return false;
        }
        f.delete();
        if (rows.isEmpty()) return false;
        setContentView(buildScoreScreen(won, local, frames, rows));
        return true;
    }

    private int parseIntSafe(String s, int def) {
        try { return Integer.parseInt(s); } catch (Exception e) { return def; }
    }

    // v0.125.001: the score screen in the launcher's own language (Kit): the shell's
    // backdrop, one centred panel, green upper-case column labels, the local player's
    // row marked like every selected row in the launcher, the total in amber, and
    // the launcher's buttons (it still had the pre-redesign look: stock Android
    // buttons on a flat navy page with a race-coloured bar).
    private View buildScoreScreen(boolean won, int local, int frames, java.util.List<String[]> rows) {
        if (kit == null) kit = new Kit(this);
        FrameLayout shell = new FrameLayout(this);
        shell.setBackgroundColor(0xFF05080A);
        final ImageView backdrop = new ImageView(this);
        backdrop.setScaleType(ImageView.ScaleType.CENTER_CROP);
        backdrop.setAlpha(0.5f);
        shell.addView(backdrop, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        final File gameDir = getExternalFilesDir(null);
        Bitmap bgBmp = PortraitStore.cachedBackground(gameDir);
        if (bgBmp != null) backdrop.setImageBitmap(bgBmp);
        else new Thread(() -> {
            final Bitmap b = PortraitStore.background(gameDir);
            if (b != null) ui.post(() -> backdrop.setImageBitmap(b));
        }, "backdrop").start();

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(kit.box(Kit.PANEL, Kit.LINE));
        panel.setPadding(dp(20), dp(18), dp(20), dp(18));

        TextView head = kit.text((won ? S.t("MISIJA ĮVYKDYTA") : S.t("MISIJA NEPAVYKO")).toUpperCase(Locale.ROOT),
                won ? Kit.LABEL : Kit.DANGER, 26);
        head.setLetterSpacing(0.12f);
        head.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        head.setGravity(Gravity.CENTER_HORIZONTAL);
        panel.addView(head);

        int secs = frames * 42 / 1000;
        TextView sub = kit.text(String.format(Locale.ROOT, "%d:%02d", secs / 60, secs % 60), Kit.TEXT2, 13);
        sub.setGravity(Gravity.CENTER_HORIZONTAL);
        sub.setPadding(0, dp(2), 0, dp(14));
        panel.addView(sub);

        // Units / Structures / Resources, the original's three categories.
        String[] headers = {S.t("Žaidėjas"), S.t("Unitai"), S.t("Pastatai"), S.t("Sunaikinta"), S.t("Prarasta"), S.t("Mineralai"), S.t("Dujos"), S.t("Taškai")};
        PortraitView probe = new PortraitView(this);
        probe.setScale(rowPortraitScale());
        probe.measure(0, 0);
        final int nameInset = dp(8) + probe.getMeasuredWidth() + dp(10);   // row padding + portrait + gap
        {
            View hdr = scoreRow(headers, true, false);
            LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            hlp.leftMargin = nameInset;
            hlp.rightMargin = dp(8);
            panel.addView(hdr, hlp);
            View rule = new View(this);
            rule.setBackgroundColor(Kit.STEEL);
            LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1)));
            rlp.bottomMargin = dp(6);
            panel.addView(rule, rlp);
        }
        for (String[] p : rows) {
            int idx = parseIntSafe(p[0], -1);
            int race = Math.max(0, Math.min(2, parseIntSafe(p[1], 1)));
            int total = parseIntSafe(p[6], 0) + parseIntSafe(p[7], 0);
            String nm = p.length > 12 && p[12] != null ? p[12] : "";
            if (nm.isEmpty()) nm = idx == local ? S.t("Tu") : "Žaidėjas " + (idx + 1);
            String who = nm + " (" + RACE_NAME[race].charAt(0) + ")";
            View rowV = scoreRow(new String[]{who, p[2], p[3], p[4], p[5], p[8], p[9], String.valueOf(total)},
                    false, idx == local);
            // Identity portrait in front of the row (id + colour from score.txt; a file
            // from an older native side has neither -> the race advisor, slot colour).
            int col = p.length > 10 ? parseIntSafe(p[10], -1) : -1;
            int por = p.length > 11 ? parseIntSafe(p[11], -1) : -1;
            if (por < 0) por = PortraitStore.aiPortrait(-1, race);
            PortraitView spv = new PortraitView(this);
            spv.setScale(rowPortraitScale());
            spv.setBorderColor(PLAYER_COLORS[(col >= 0 && col < PLAYER_COLORS.length ? col : Math.max(0, idx)) % PLAYER_COLORS.length]);
            spv.setPortrait(gameDir, por);
            if (idx == local && won) spv.setTalkLoop(true);   // the winner talks
            LinearLayout wrap = new LinearLayout(this);
            wrap.setOrientation(LinearLayout.HORIZONTAL);
            wrap.setGravity(Gravity.CENTER_VERTICAL);
            // the local player's row: the launcher's "selected" look
            wrap.setBackground(kit.box(idx == local ? Kit.SEL_BG : 0x00000000, idx == local ? Kit.LABEL : 0x00000000));
            wrap.setPadding(dp(8), dp(5), dp(8), dp(5));
            wrap.addView(spv);
            LinearLayout.LayoutParams rvp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            rvp.leftMargin = dp(10);
            wrap.addView(rowV, rvp);
            LinearLayout.LayoutParams wlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            wlp.bottomMargin = dp(4);
            panel.addView(wrap, wlp);
        }

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.END);
        actions.setPadding(0, dp(18), 0, 0);
        final File last = new File(gameDir, "replays/LastGame.rep");
        final TextView[] saveRef = new TextView[1];
        final TextView save = kit.button(S.t("Išsaugoti replay"), 0, () -> {
            if (!last.isFile()) return;
            String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT)
                    .format(new java.util.Date(last.lastModified()));
            File dst = new File(last.getParentFile(), stamp + ".rep");
            for (int k = 2; dst.exists() && k < 100; ++k) dst = new File(last.getParentFile(), stamp + "-" + k + ".rep");
            if (last.renameTo(dst)) {
                saveRef[0].setEnabled(false);
                saveRef[0].setAlpha(0.4f);
                Toast.makeText(this, S.t("Išsaugota: ") + dst.getName(), Toast.LENGTH_LONG).show();
            } else {
                Toast.makeText(this, S.t("Nepavyko išsaugoti replay."), Toast.LENGTH_LONG).show();
            }
        });
        saveRef[0] = save;
        if (!last.isFile()) { save.setEnabled(false); save.setAlpha(0.4f); }
        actions.addView(save, new LinearLayout.LayoutParams(dp(220), dp(48)));
        TextView ret = kit.button(S.t("Grįžti"), 1, this::recreate);
        LinearLayout.LayoutParams retLp = new LinearLayout.LayoutParams(dp(220), dp(48));
        retLp.leftMargin = dp(12);
        actions.addView(ret, retLp);
        panel.addView(actions);

        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        FrameLayout holder = new FrameLayout(this);
        int screenW = getResources().getDisplayMetrics().widthPixels;
        int w = Math.min(screenW - dp(32), dp(1100));
        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        plp.topMargin = dp(16);
        plp.bottomMargin = dp(16);
        holder.addView(panel, plp);
        sv.addView(holder, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        shell.addView(sv, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        return shell;
    }

    private View scoreRow(String[] cells, boolean header, boolean local) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(header ? 4 : 0), 0, dp(header ? 6 : 0));
        for (int i = 0; i < cells.length; ++i) {
            final boolean last = i == cells.length - 1;
            String s = cells[i] == null ? "-" : cells[i];
            TextView t;
            if (header) {
                t = kit.text(s.toUpperCase(Locale.ROOT), Kit.LABEL, 12);
                t.setLetterSpacing(0.12f);
            } else {
                t = kit.text(s, last ? Kit.ACCENT : i == 0 ? (local ? Color.WHITE : Kit.TEXT) : Kit.TEXT, i == 0 || last ? 16 : 15);
                if (last || (i == 0 && local)) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            }
            t.setSingleLine(true);
            t.setGravity(i == 0 ? Gravity.START : Gravity.END);
            t.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, i == 0 ? 2.4f : 1f));
            row.addView(t);
        }
        return row;
    }

    private TextView emptyLabel(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(Color.GRAY);
        return t;
    }

    private LinearLayout cardBase(int color) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        int p = dp(10);
        card.setPadding(p, p, p, p);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(dp(8));
        card.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(6);
        card.setLayoutParams(lp);
        return card;
    }

    // Selection highlight: a map list row is flat (tag MAP_ROW), anything else a card.
    private void paintCard(View card, int color, boolean on) {
        if (card.getTag() == MAP_ROW) {
            TextView t = (TextView) ((ViewGroup) card).getChildAt(0);
            t.setBackgroundColor(on ? Kit.SEL_BG : 0x00000000);
            t.setTextColor(on ? Color.WHITE : Kit.TEXT);
            return;
        }
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(dp(8));
        card.setBackground(bg);
    }

    private LinearLayout mapRow(String name) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setTag(MAP_ROW);
        TextView t = kit.text(name, Kit.TEXT, 15);
        t.setPadding(dp(10), dp(11), dp(10), dp(11));
        t.setSingleLine(true);
        t.setEllipsize(android.text.TextUtils.TruncateAt.END);
        row.addView(t, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        View line = new View(this);
        line.setBackgroundColor(0xFF3A1414);
        row.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
        return row;
    }

    private void select(View card, int selColor, boolean isMap, File f) {
        if (selectedCard != null) paintCard(selectedCard, selectedCardDefaultColor, false);
        selectedCard = card;
        selectedCardDefaultColor = isMap ? COL_MAP : COL_CARD;
        paintCard(card, selColor, true);
        // Map and replay selection are mutually exclusive.
        selectedMap = isMap ? f : null;
        selectedReplay = isMap ? null : f;
        if (isMap && selectedMap != null) {
            // Picking an imported file map clears any bundled-MPQ selection.
            selectedMpqMap = null;
        }
        updateActionSummary();
    }

    // Fills a container with the imported + bundled (MPQ) map cards, grouped by
    // section. Shared by the Single Player and Multiplayer map lists.
    // The BW convention is a leading "(N)" in the map/file name = max players.
    private int mapMaxPlayers(String name) {
        int i = name.indexOf('(');
        if (i < 0) return 0;
        int j = name.indexOf(')', i);
        if (j <= i) return 0;
        try { return Integer.parseInt(name.substring(i + 1, j).trim()); } catch (Exception e) { return 0; }
    }

    private void populateMapList(LinearLayout container) {
        container.removeAllViews();
        // #3.4: hide maps that can't seat the chosen player count (unknown = keep).
        int want = clampCount(prefs().getInt("playerCount", 2));
        boolean shown = false;
        int listed = 0;
        File[] maps = new File(getExternalFilesDir(null), "maps").listFiles((d, n) -> {
            String l = n.toLowerCase(Locale.ROOT);
            return l.endsWith(".scm") || l.endsWith(".scx");
        });
        if (maps != null && maps.length > 0) {
            Arrays.sort(maps);
            boolean hdr = false;
            for (File map : maps) {
                int mx = mapMaxPlayers(map.getName());
                if (mx > 0 && mx < want) continue;
                if (!hdr) { container.addView(sectionHeader("— " + S.t("Importuoti") + " —")); hdr = true; }
                container.addView(mapCard(map));
                shown = true; ++listed;
            }
        }
        // v0.83.033: maps from active map-pack mods (folder maps/ inside the mod).
        java.util.List<File> modMaps = activeModMaps();
        if (!modMaps.isEmpty()) {
            java.util.Collections.sort(modMaps);
            boolean hdr = false;
            for (File map : modMaps) {
                int mx = mapMaxPlayers(map.getName());
                if (mx > 0 && mx < want) continue;
                if (!hdr) { container.addView(sectionHeader("— " + S.t("Modų žemėlapiai") + " —")); hdr = true; }
                container.addView(mapCard(map));
                shown = true; ++listed;
            }
        }
        boolean cd = new File(getExternalFilesDir(null), "StarCraft.mpq").exists()
                || new File(getExternalFilesDir(null), "BroodWar.mpq").exists();
        if (cd) {
            for (Episode grp : bundledMapGroups()) {
                boolean hdr = false;
                for (String[] m : grp.missions) {
                    int mx = mapMaxPlayers(m[0]);
                    if (mx > 0 && mx < want) continue;
                    if (!hdr) { container.addView(sectionHeader("— " + grp.title + " —")); hdr = true; }
                    container.addView(bundledMapCard(m[0], m[1]));
                    shown = true; ++listed;
                }
            }
        }
        if (!shown) {
            container.addView(emptyLabel(S.t("(nėra ") + want + "+ žaidėjų žemėlapių — importuokite arba mažinkite žaidėjų skaičių)"));
        }
        TextView cl = container == mapList ? mapCountLabel : container == mapListMp ? mpMapCountLabel : null;
        if (cl != null)
            cl.setText((S.t("Žemėlapiai") + "  ·  " + listed).toUpperCase(Locale.ROOT));
    }

    private View mapCard(File map) {
        final LinearLayout card = mapRow(map.getName());
        if (map.equals(selectedMap)) select(card, COL_MAP_SEL, true, map);
        card.setOnClickListener(v -> {
            select(card, COL_MAP_SEL, true, map);
            showSpPreview(map.getName(), false, map.getAbsolutePath());
        });
        card.setOnLongClickListener(v -> { showMinimapPreview(map.getName(), false, map.getAbsolutePath()); return true; });
        return card;
    }

    // A map preview panel (single player, LAN create / join / lobby): the map's minimap
    // (native, off the UI thread), size, seats and its scenario description.
    private final class PreviewPane {
        ImageView img;
        TextView name, info, desc;
        int seq;

        LinearLayout build(String placeholder) {
            LinearLayout pin = new LinearLayout(SetupActivity.this);
            pin.setOrientation(LinearLayout.VERTICAL);
            img = new ImageView(SetupActivity.this);
            img.setAdjustViewBounds(true);
            img.setScaleType(ImageView.ScaleType.FIT_CENTER);
            img.setBackground(kit.box(0xFF05080A, Kit.STEEL));
            int ip = Math.max(1, dp(1));
            img.setPadding(ip, ip, ip, ip);
            img.setVisibility(View.GONE);          // no empty frame before there is a map
            pin.addView(img, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            name = kit.text(placeholder, Color.WHITE, 17);
            name.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
            name.setPadding(0, dp(10), 0, dp(2));
            pin.addView(name);
            info = kit.text("", Kit.TEXT2, 12);
            pin.addView(info);
            desc = kit.text("", Kit.TEXT2, 12);
            desc.setPadding(0, dp(8), 0, 0);
            pin.addView(desc);
            return pin;
        }

        // In its own nested scroller, filling a column.
        void addTo(LinearLayout col, String placeholder) {
            ScrollView sv = new ScrollView(SetupActivity.this);
            sv.setNestedScrollingEnabled(true);
            sv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
            sv.addView(build(placeholder));
            col.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        }

        void clear(String text) {
            ++seq;
            img.setImageBitmap(null);
            img.setVisibility(View.GONE);
            name.setText(text);
            info.setText("");
            desc.setText("");
        }

        void show(final String title, final boolean isInternal, final String mapArg) {
            final int my = ++seq;
            name.setText(title);
            info.setText("");
            desc.setText("");
            if (!minimapLibOk) { desc.setText(S.t("Peržiūra neprieinama")); return; }
            final String gameDir = getExternalFilesDir(null).getAbsolutePath();
            // one scratch file per pane: two panes may render at the same time
            final File out = new File(getCacheDir(), "preview_" + System.identityHashCode(this) + ".png");
            final String scratch = new File(getCacheDir(), "preview_" + System.identityHashCode(this) + ".tmp").getAbsolutePath();
            minimapExec.execute(() -> {
                if (my != seq) return;   // a newer pick is already queued
                String err, mi = null;
                try {
                    err = isInternal ? nativeGenMinimapInternal(gameDir, mapArg, out.getAbsolutePath())
                                     : nativeGenMinimap(gameDir, mapArg, out.getAbsolutePath());
                } catch (Throwable t) { err = String.valueOf(t.getMessage()); }
                try { mi = isInternal ? nativeMapInfoInternal(gameDir, mapArg, scratch) : nativeMapInfo(mapArg); }
                catch (Throwable ignored) {}
                final Bitmap bmp = err == null && out.exists() ? android.graphics.BitmapFactory.decodeFile(out.getAbsolutePath()) : null;
                final String d = parseMapDesc(mi);
                ui.post(() -> {
                    if (my != seq) return;
                    img.setImageBitmap(bmp);
                    img.setVisibility(bmp != null ? View.VISIBLE : View.GONE);
                    // nearest-neighbour: one pixel per map tile must stay crisp when scaled up
                    if (bmp != null && img.getDrawable() != null) img.getDrawable().setFilterBitmap(false);
                    info.setText(bmp != null ? bmp.getWidth() + " × " + bmp.getHeight() : "");
                    desc.setText(d);
                });
            });
        }

        // By map id as the lobby names it: an MPQ path (has '/') or an imported file name.
        void showMapId(String id) {
            if (id == null || id.isEmpty()) { clear(""); return; }
            if (id.indexOf('/') >= 0) {
                String title = mapBasename(id);
                for (Episode grp : bundledMapGroups()) for (String[] m : grp.missions) if (m[1].equals(id)) title = m[0];
                show(title, true, id);
            } else {
                File f = resolveMapFile(id);
                if (f != null) show(id, false, f.getAbsolutePath());
                else clear(id);
            }
        }
    }

    // The map selection is shared by the single-player and the LAN Create lists.
    private void showSpPreview(String title, boolean isInternal, String mapArg) {
        if (spPane != null) spPane.show(title, isInternal, mapArg);
        if (mpPane != null) mpPane.show(title, isInternal, mapArg);
    }

    // v0.53: #7 map preview. Long-pressing a map card renders its minimap to a
    // PNG (native, off the UI thread) and shows it in a dialog. isInternal =
    // the map lives inside the game MPQs (bundled/campaign), addressed by its
    // archive path; otherwise mapArg is a filesystem path.
    private void showMinimapPreview(String title, boolean isInternal, String mapArg) {
        if (!minimapLibOk) {
            Toast.makeText(this, S.t("Peržiūra neprieinama"), Toast.LENGTH_SHORT).show();
            return;
        }
        final String gameDir = getExternalFilesDir(null).getAbsolutePath();
        final File out = new File(getCacheDir(), "mm_preview.png");
        final Toast wait = Toast.makeText(this, "Generuojama peržiūra…", Toast.LENGTH_SHORT);
        wait.show();
        minimapExec.execute(() -> {
            String err;
            try {
                err = isInternal
                        ? nativeGenMinimapInternal(gameDir, mapArg, out.getAbsolutePath())
                        : nativeGenMinimap(gameDir, mapArg, out.getAbsolutePath());
            } catch (Throwable t) {
                err = String.valueOf(t.getMessage());
            }
            final String ferr = err;
            ui.post(() -> {
                wait.cancel();
                if (ferr != null || !out.exists()) {
                    Toast.makeText(this, "Peržiūra nepavyko: " + (ferr == null ? "?" : ferr), Toast.LENGTH_LONG).show();
                    return;
                }
                android.graphics.Bitmap bmp = android.graphics.BitmapFactory.decodeFile(out.getAbsolutePath());
                if (bmp == null) { Toast.makeText(this, "Peržiūra nepavyko (dekodavimas)", Toast.LENGTH_SHORT).show(); return; }
                ImageView iv = new ImageView(this);
                iv.setImageBitmap(bmp);
                iv.setAdjustViewBounds(true);
                iv.setScaleType(ImageView.ScaleType.FIT_CENTER);
                // Nearest-neighbour so the small minimap stays crisp when scaled up.
                iv.getDrawable().setFilterBitmap(false);
                int pad = dp(12);
                iv.setPadding(pad, pad, pad, pad);
                iv.setMinimumWidth(dp(280));
                iv.setMinimumHeight(dp(280));
                new AlertDialog.Builder(this)
                        .setTitle(title)
                        .setView(iv)
                        .setPositiveButton(S.t("Uždaryti"), null)
                        .show();
            });
        });
    }

    private TextView sectionHeader(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(Kit.ACCENT);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        t.setPadding(dp(4), dp(10), dp(2), dp(4));
        return t;
    }

    // A bundled MPQ map row (same look as an imported map card). Selecting it
    // sets selectedMpqMap and clears any imported-file selection.
    private View bundledMapCard(String name, String path) {
        final LinearLayout card = mapRow(name);
        if (path.equals(selectedMpqMap)) selectBundled(card, name, path);
        card.setOnClickListener(v -> {
            selectBundled(card, name, path);
            showSpPreview(name, true, path);
        });
        card.setOnLongClickListener(v -> { showMinimapPreview(name, true, path); return true; });
        return card;
    }

    private void selectBundled(View card, String name, String path) {
        if (selectedCard != null) paintCard(selectedCard, selectedCardDefaultColor, false);
        selectedCard = card;
        selectedCardDefaultColor = COL_MAP;
        paintCard(card, COL_MAP_SEL, true);
        selectedMpqMap = path;
        selectedMpqMapName = name;
        selectedMap = null;    // mutually exclusive with an imported file map
        selectedReplay = null;
        updateActionSummary();
    }

    // ---- REPLAY tab (v0.68 item 15) ------------------------------------
    // Same master-detail shape as the MOD tab: list on the left, details and
    // the action buttons on the right. START GAME is not used here any more --
    // ŽIŪRĖTI lives in the detail pane.
    private LinearLayout replayListBox, replayDetailBox;
    private EditText replaySearch;
    private LinearLayout saveListBox, saveDetailBox; // v0.86.001: resumable disk saves, own tab
    private File selectedSave;
    private final java.util.Map<String, ReplayInfo> replayInfoCache = new java.util.HashMap<>();

    // A list | detail page (saves, replays): two panels that fill the page panel on
    // wide screens, stacked on narrow ones. Returns {list box, detail box}.
    private LinearLayout[] listDetailPage(LinearLayout root, int topRowPx) {
        final boolean wide = getResources().getConfiguration().screenWidthDp >= 600;
        LinearLayout cols = new LinearLayout(this);
        cols.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        cols.setBaselineAligned(false);
        LinearLayout listPanel = spPanel(), detailPanel = spPanel();
        detailPanel.setPadding(dp(14), dp(12), dp(14), dp(12));
        LinearLayout list = new LinearLayout(this), detail = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        detail.setOrientation(LinearLayout.VERTICAL);
        if (wide) {
            for (int i = 0; i != 2; ++i) {
                ScrollView sv = new ScrollView(this);
                sv.setNestedScrollingEnabled(true);
                sv.setOnTouchListener((v, e) -> { v.getParent().requestDisallowInterceptTouchEvent(true); return false; });
                sv.addView(i == 0 ? list : detail);
                (i == 0 ? listPanel : detailPanel).addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
            }
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.55f);
            l1.rightMargin = dp(10);
            cols.addView(listPanel, l1);
            cols.addView(detailPanel, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
            root.addView(cols, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(430)));
            fitToPanel(cols, topRowPx);
        } else {
            listPanel.addView(list);
            detailPanel.addView(detail);
            LinearLayout.LayoutParams l1 = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            l1.bottomMargin = dp(10);
            cols.addView(listPanel, l1);
            cols.addView(detailPanel, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            root.addView(cols);
        }
        return new LinearLayout[]{list, detail};
    }

    // A list row: portrait | title + subtitle | optional tag | right-hand note.
    private LinearLayout listRow(int portraitId, int frameColor, String title, String sub, String tag, int tagColor, String right, boolean selected) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(8), dp(8), dp(10), dp(8));
        row.setBackground(selected ? kit.box(Kit.SEL_BG, Kit.LABEL) : kit.box(0xFF070B0E, 0xFF2A3338));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(6);
        row.setLayoutParams(lp);
        PortraitView pv = new PortraitView(this);
        pv.setScale(rowPortraitScale());
        pv.setBorderColor(frameColor);
        pv.setAnimate(selected);          // a long list: only the picked row moves
        pv.setPortrait(getExternalFilesDir(null), portraitId);
        row.addView(pv);
        LinearLayout mid = new LinearLayout(this);
        mid.setOrientation(LinearLayout.VERTICAL);
        mid.setPadding(dp(12), 0, dp(8), 0);
        TextView t = kit.text(title, Color.WHITE, 15);
        t.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        t.setSingleLine(true);
        t.setEllipsize(android.text.TextUtils.TruncateAt.END);
        mid.addView(t);
        TextView st = kit.text(sub, Kit.TEXT2, 12);
        st.setSingleLine(true);
        st.setEllipsize(android.text.TextUtils.TruncateAt.END);
        st.setTag("sub");
        mid.addView(st);
        row.addView(mid, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        if (tag != null) {
            TextView g = kit.text(tag, tagColor, 11);
            g.setBackground(kit.box(0x00000000, tagColor));
            g.setPadding(dp(6), dp(1), dp(6), dp(1));
            row.addView(g);
        }
        if (right != null) {
            TextView r = kit.text(right, Kit.TEXT2, 12);
            r.setPadding(dp(10), 0, 0, 0);
            row.addView(r);
        }
        return row;
    }

    // "Label   value" lines of a detail panel.
    private View detailRow(String label, String value) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(3), 0, dp(3));
        row.addView(kit.text(label, Kit.TEXT2, 13), new LinearLayout.LayoutParams(dp(110), ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(kit.text(value, Kit.TEXT, 13), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return row;
    }

    private void emptyDetail(LinearLayout box, String text) {
        if (box == null) return;
        box.removeAllViews();
        box.addView(kit.text(text, Kit.TEXT2, 13));
    }

    private String fileStamp(File f) {
        return new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(new java.util.Date(f.lastModified()));
    }

    // Resumable disk saves (their own screen: saves and replays are different things).
    // Each one relaunches the game it was saved from (see OpenBWActivity.readSaveArgs).
    private void buildSavePage(LinearLayout root) {
        LinearLayout[] ld = listDetailPage(root, 0);
        saveListBox = ld[0];
        saveDetailBox = ld[1];
        renderSaveList();
    }

    private void buildReplayPage(LinearLayout root) {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(0, 0, 0, dp(8));
        replaySearch = new EditText(this);
        replaySearch.setHint(S.t("Paieška: vardas, žemėlapis, žaidėjas…"));
        replaySearch.setTextColor(Kit.TEXT);
        replaySearch.setHintTextColor(Kit.TEXT2);
        replaySearch.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        replaySearch.setSingleLine(true);
        replaySearch.setBackground(kit.box(0xFF05080A, Kit.STEEL));
        replaySearch.setPadding(dp(10), 0, dp(10), 0);
        replaySearch.addTextChangedListener(new android.text.TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) {}
            public void afterTextChanged(android.text.Editable e) { renderReplayList(); }
        });
        bar.addView(replaySearch, new LinearLayout.LayoutParams(0, dp(44), 1f));
        LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(44));
        ilp.leftMargin = dp(8);
        bar.addView(kit.smallButton(S.t("+ Importuoti (.rep)").toUpperCase(Locale.ROOT), () -> pickFiles(REQ_PICK_REPLAY)), ilp);
        root.addView(bar);
        LinearLayout[] ld = listDetailPage(root, dp(52));
        replayListBox = ld[0];
        replayDetailBox = ld[1];
        renderReplayList();
        showReplayPlaceholder();
    }

    private void showReplayPlaceholder() {
        emptyDetail(replayDetailBox, S.t("Pasirink replay iš sąrašo kairėje."));
    }

    private File[] replayFiles() {
        File[] reps = new File(getExternalFilesDir(null), "replays").listFiles(
                (d, n) -> n.toLowerCase(Locale.ROOT).endsWith(".rep"));
        if (reps == null) return new File[0];
        // Newest first: after a match the fresh recording should be at the top.
        Arrays.sort(reps, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return reps;
    }

    // --- v0.86 disk save list -------------------------------------------

    private File[] saveFiles() {
        File[] s = new File(getExternalFilesDir(null), "saves").listFiles(
                (d, n) -> n.toLowerCase(Locale.ROOT).endsWith(".sav"));
        if (s == null) return new File[0];
        Arrays.sort(s, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return s;
    }

    // "<map>  ·  m:ss" read from a .sav container header (map name + frame).
    // Mirrors ui_functions::disk_save layout; returns null on a bad/short file.
    private String saveLabel(File f) {
        java.io.RandomAccessFile r = null;
        try {
            r = new java.io.RandomAccessFile(f, "r");
            byte[] hdr = new byte[24];
            if (r.read(hdr) != 24) return null;
            if (leU32(hdr, 0) != 0x53564153L || leU32(hdr, 4) != 1L) return null; // "SAVS", v1
            long frame = leU32(hdr, 8);
            long nameLen = leU32(hdr, 20);
            String name = "?";
            if (nameLen > 0 && nameLen < 4096) {
                byte[] nb = new byte[(int) nameLen];
                if (r.read(nb) == nameLen) name = new String(nb, "UTF-8");
            }
            long secs = frame * 42 / 1000; // ~42ms/frame at normal speed
            return name + "  ·  " + String.format(Locale.ROOT, "%d:%02d", secs / 60, secs % 60);
        } catch (Throwable t) {
            return null;
        } finally {
            if (r != null) try { r.close(); } catch (Throwable ignored) {}
        }
    }

    // D5 free save-name: read the plain-text "<slot>.name" sidecar next to a .sav.
    private String saveSidecarName(File sav) {
        File nm = new File(sav.getParentFile(), sav.getName().replaceFirst("\\.sav$", ".name"));
        if (!nm.isFile()) return null;
        java.io.FileInputStream in = null;
        try {
            byte[] b = new byte[(int) Math.min(nm.length(), 128)];
            in = new java.io.FileInputStream(nm);
            int n = in.read(b);
            if (n <= 0) return null;
            String s = new String(b, 0, n, "UTF-8").trim();
            return s.isEmpty() ? null : s;
        } catch (Throwable t) {
            return null;
        } finally {
            if (in != null) try { in.close(); } catch (Throwable ignored) {}
        }
    }

    private static long leU32(byte[] b, int o) {
        return (b[o] & 0xffL) | ((b[o + 1] & 0xffL) << 8) | ((b[o + 2] & 0xffL) << 16) | ((b[o + 3] & 0xffL) << 24);
    }

    // --- Save/load submilestone 2: campaign vs single-player categories + names --
    // A save's kind is the slot-file prefix set in submilestone 1 (cslotN = campaign,
    // slotN = single-player). Its human title comes from the container's relaunch args
    // (the same argv the launcher uses to resume it): a campaign dir -> Episode +
    // Mission; a melee -> NvN race-vs-race. Date/time is the file's mtime.
    private boolean saveIsCampaign(File f) {
        return f.getName().toLowerCase(Locale.ROOT).startsWith("cslot");
    }

    // Read the '\n'-joined relaunch args from a .sav container (right after the name).
    private String[] readSaveArgs(File f) {
        java.io.RandomAccessFile r = null;
        try {
            r = new java.io.RandomAccessFile(f, "r");
            byte[] h = new byte[24];
            if (r.read(h) != 24) return null;
            if (leU32(h, 0) != 0x53564153L || leU32(h, 4) != 1L) return null; // "SAVS", v1
            long nameLen = leU32(h, 20);
            if (nameLen < 0 || nameLen > 4096) return null;
            r.seek(24 + nameLen);
            byte[] al = new byte[4];
            if (r.read(al) != 4) return null;
            long argsLen = leU32(al, 0);
            if (argsLen <= 0 || argsLen > 65536) return new String[0];
            byte[] ab = new byte[(int) argsLen];
            if (r.read(ab) != argsLen) return null;
            return new String(ab, "UTF-8").split("\n");
        } catch (Throwable t) {
            return null;
        } finally {
            if (r != null) try { r.close(); } catch (Throwable ignored) {}
        }
    }

    private String argValue(String[] args, String key) {
        if (args == null) return null;
        for (int i = 0; i + 1 < args.length; ++i) if (args[i].equals(key)) return args[i + 1];
        return null;
    }
    private String raceName(int r) {
        switch (r) {
            case 0: return "Zerg";
            case 1: return "Terran";
            case 2: return "Protoss";
            default: return S.t("Atsitiktinė");
        }
    }
    // "Episode — Mission" from the --campaign dir; falls back to the raw dir.
    private String campaignTitle(String dir) {
        if (dir == null) return S.t("Kampanija");
        for (Episode ep : campaignEpisodes())
            for (String[] m : ep.missions)
                if (m[1].equalsIgnoreCase(dir)) return ep.title + " · " + m[0];
        return dir;
    }
    // "1v1 · Terran vs Zerg" (2 players) or "Np · <race>" (more).
    private String spTitle(String[] args) {
        int players = parseIntSafe(argValue(args, "--players"), 2);
        int r0 = parseIntSafe(argValue(args, "--race"), 5);
        int r1 = parseIntSafe(argValue(args, "--enemy-race"), 5);
        if (players == 2) return "1v1 · " + raceName(r0) + " vs " + raceName(r1);
        return players + "p · " + raceName(r0);
    }
    // Categorised display title for a save (kind decided by the slot prefix).
    private String saveDisplayTitle(File sv) {
        String[] args = readSaveArgs(sv);
        return saveIsCampaign(sv) ? campaignTitle(argValue(args, "--campaign")) : spTitle(args);
    }

    private void renderSaveList() {
        if (saveListBox == null) return;
        saveListBox.removeAllViews();
        // Two separate categories so campaign and single-player saves never mix in one
        // list. Kind = slot-file prefix.
        java.util.ArrayList<File> camp = new java.util.ArrayList<>();
        java.util.ArrayList<File> sp = new java.util.ArrayList<>();
        boolean stillThere = false;
        for (File sv : saveFiles()) {
            if (saveLabel(sv) == null) continue; // skip unreadable/foreign containers
            (saveIsCampaign(sv) ? camp : sp).add(sv);
            if (sv.equals(selectedSave)) stillThere = true;
        }
        if (!stillThere) selectedSave = null;
        int shown = 0;
        shown += renderSaveCategory(S.t("Kampanija"), camp, true);
        shown += renderSaveCategory(S.t("Vieno žaidėjo"), sp, false);
        if (shown == 0) saveListBox.addView(emptyLabel(S.t("(nėra išsaugotų žaidimų)")));
        showSaveDetail();
        updateStartButtonVisibility();
    }

    // The save's local race (for its portrait): the campaign dir's race, or --race.
    private int saveRace(File sv, String[] args) {
        if (saveIsCampaign(sv)) {
            String d = argValue(args, "--campaign");
            if (d == null) return -1;
            return d.contains("zerg") ? 0 : d.contains("protoss") ? 2 : 1;
        }
        return parseIntSafe(argValue(args, "--race"), -1);
    }

    // Render one category (header + rows). Campaign titles = Episode + Mission;
    // single-player = NvN race-vs-race. The file's date/time sits on the right.
    private int renderSaveCategory(String header, java.util.List<File> files, boolean campaign) {
        if (files.isEmpty()) return 0;
        TextView h = kit.text((header + "  ·  " + files.size()).toUpperCase(Locale.ROOT), Kit.ACCENT, 11);
        h.setLetterSpacing(0.1f);
        h.setPadding(dp(4), dp(6), dp(4), dp(6));
        saveListBox.addView(h);
        for (final File sv : files) {
            String title = saveDisplayTitle(sv);
            String user = saveSidecarName(sv);
            int race = saveRace(sv, readSaveArgs(sv));
            // If the player named the save, the derived title moves to the subtitle line;
            // otherwise the subtitle is the map and the game time.
            String mapTime = saveLabel(sv);
            LinearLayout row = listRow(PortraitStore.aiPortrait(-1, race), race >= 0 && race <= 2 ? RACE_COLOR[race] : Kit.STEEL,
                    user != null ? user : title, user != null ? title : (mapTime != null ? mapTime : ""), null, 0, fileStamp(sv), sv.equals(selectedSave));
            row.setOnClickListener(v -> { selectedSave = sv; renderSaveList(); });
            saveListBox.addView(row);
        }
        return files.size();
    }

    private void showSaveDetail() {
        if (saveDetailBox == null) return;
        final File sv = selectedSave;
        if (sv == null) { emptyDetail(saveDetailBox, S.t("Pasirink išsaugotą žaidimą kairėje.")); return; }
        saveDetailBox.removeAllViews();
        String user = saveSidecarName(sv);
        String[] args = readSaveArgs(sv);
        TextView t = kit.text(user != null ? user : saveDisplayTitle(sv), Color.WHITE, 22);
        t.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        t.setPadding(0, 0, 0, dp(10));
        saveDetailBox.addView(t);
        // what the save is: only when the title above is the player's own name for it
        // (an unnamed save's title already says it)
        if (user != null) {
            if (saveIsCampaign(sv)) {
                String dir = argValue(args, "--campaign");
                String epT = "?", mi = dir == null ? "?" : dir;
                if (dir != null) for (Episode ep : campaignEpisodes()) for (String[] m : ep.missions)
                    if (m[1].equalsIgnoreCase(dir)) { epT = ep.title; mi = m[0]; }
                saveDetailBox.addView(detailRow(S.t("Kampanija"), epT));
                saveDetailBox.addView(detailRow(S.t("Misija"), mi));
            } else {
                saveDetailBox.addView(detailRow(S.t("Žaidimas"), spTitle(args)));
            }
        }
        String lbl = saveLabel(sv);      // "<map>  ·  m:ss"
        if (lbl != null) {
            int dot = lbl.lastIndexOf("  ·  ");
            saveDetailBox.addView(detailRow(S.t("Žemėlapis"), dot >= 0 ? lbl.substring(0, dot) : lbl));
            if (dot >= 0) saveDetailBox.addView(detailRow(S.t("Žaidimo laikas"), lbl.substring(dot + 5)));
        }
        saveDetailBox.addView(detailRow(S.t("Išsaugota"), fileStamp(sv)));
    }

    private void loadSave(File sv) {
        // Loading a CAMPAIGN save must set the campaignDir pref before launching,
        // exactly like the normal Start/Repeat path. Completion is recorded in onResume
        // from getCampaignDir(), so without this a mission beaten from a save never got
        // its checkmark. The mission dir lives in the save's own relaunch args
        // ("--campaign campaign/<race>/<m>").
        if (saveIsCampaign(sv)) {
            String cdir = argValue(readSaveArgs(sv), "--campaign");
            if (cdir != null && !cdir.isEmpty()) setCampaignDir(cdir);
        }
        Intent i = new Intent(this, OpenBWActivity.class);
        i.putExtra(OpenBWActivity.EXTRA_LOAD_SAVE, sv.getAbsolutePath());
        startActivity(i);
    }

    private void confirmDeleteSave(final File sv) {
        new AlertDialog.Builder(this)
                .setTitle(S.t("Pašalinti išsaugotą žaidimą?"))
                .setMessage(saveSidecarName(sv) != null ? saveSidecarName(sv) : saveDisplayTitle(sv))
                .setPositiveButton(S.t("Pašalinti"), (d, w) -> {
                    if (sv.delete()) {
                        new File(sv.getParentFile(), sv.getName().replaceFirst("\\.sav$", ".name")).delete();
                        if (sv.equals(selectedSave)) selectedSave = null;
                        renderSaveList();
                    } else Toast.makeText(this, S.t("Nepavyko pašalinti."), Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }

    // Header of a replay, parsed once per file version (a native call) and off the UI thread.
    private ReplayInfo cachedReplayInfo(File rep) {
        return replayInfoCache.get(rep.getAbsolutePath() + "|" + rep.lastModified() + "|" + rep.length());
    }
    private void replayInfoAsync(final File rep, final Runnable then) {
        final String key = rep.getAbsolutePath() + "|" + rep.lastModified() + "|" + rep.length();
        if (replayInfoCache.containsKey(key)) { then.run(); return; }
        minimapExec.execute(() -> {
            final ReplayInfo info = ReplayInfo.parse(rep.getAbsolutePath());
            ui.post(() -> { replayInfoCache.put(key, info); then.run(); });
        });
    }
    private String replaySummary(ReplayInfo info) {
        if (info == null) return "";
        if (info.error != null) return S.t("Antraštė nenuskaityta");
        StringBuilder who = new StringBuilder();
        for (ReplayInfo.Player p : info.players) {
            if (who.length() > 0) who.append(" vs ");
            who.append(p.computer ? "AI" : p.name).append(" (").append(p.raceLetter()).append(")");
        }
        return (info.mapName.isEmpty() ? "?" : info.mapName) + "  ·  " + who + "  ·  " + info.duration();
    }
    private boolean replayMatches(File rep, String q) {
        if (q.isEmpty() || rep.getName().toLowerCase(Locale.ROOT).contains(q)) return true;
        ReplayInfo info = cachedReplayInfo(rep);
        if (info == null || info.error != null) return false;
        if (info.mapName.toLowerCase(Locale.ROOT).contains(q)) return true;
        for (ReplayInfo.Player p : info.players) if (p.name.toLowerCase(Locale.ROOT).contains(q)) return true;
        return false;
    }

    private void renderReplayList() {
        if (replayListBox == null) return;
        replayListBox.removeAllViews();
        String q = replaySearch == null ? "" : replaySearch.getText().toString().trim().toLowerCase(Locale.ROOT);
        File[] reps = replayFiles();
        int shown = 0;
        boolean lastHdr = false, savedHdr = false;
        int saved = 0;
        for (File rep : reps) if (!rep.getName().equalsIgnoreCase("LastGame.rep")) ++saved;
        for (int pass = 0; pass != 2; ++pass) {
            for (File rep : reps) {
                boolean last = rep.getName().equalsIgnoreCase("LastGame.rep");
                if (last != (pass == 0)) continue;
                if (!replayMatches(rep, q)) continue;
                if (last && !lastHdr) { replayListBox.addView(replaySection(S.t("Paskutinis žaidimas"))); lastHdr = true; }
                if (!last && !savedHdr) { replayListBox.addView(replaySection(S.t("Išsaugoti replay'ai") + "  ·  " + saved)); savedHdr = true; }
                replayListBox.addView(replayCard(rep));
                ++shown;
            }
        }
        if (shown == 0) {
            replayListBox.addView(emptyLabel(reps.length == 0
                    ? S.t("(nėra replay failų)") : S.t("(nėra atitinkančių paieškai)")));
        }
        updateStartButtonVisibility();
    }
    private View replaySection(String s) {
        TextView h = kit.text(s.toUpperCase(Locale.ROOT), Kit.ACCENT, 11);
        h.setLetterSpacing(0.1f);
        h.setPadding(dp(4), dp(6), dp(4), dp(6));
        return h;
    }

    private void selectReplayFile(File rep) {
        selectedReplay = rep;
        renderReplayList();
        if (rep != null && rep.exists()) showReplayDetail(rep); else showReplayPlaceholder();
    }

    // The row is cheap to build: name and date at once; the header line (map,
    // players, length -- a native parse per file) arrives from the background.
    private View replayCard(final File rep) {
        ReplayInfo info = cachedReplayInfo(rep);
        boolean last = rep.getName().equalsIgnoreCase("LastGame.rep");
        String tag = null;
        int tagColor = Kit.ACCENT;
        int portrait = -1, frame = Kit.STEEL;
        if (info != null) {
            if (info.error != null) tag = S.t("klaida");
            else {
                if (info.hasComputer) { tag = S.t("prieš AI"); tagColor = Kit.DANGER; }
                for (ReplayInfo.Player p : info.players) if (!p.computer) {
                    portrait = PortraitStore.aiPortrait(-1, p.race);
                    if (p.race >= 0 && p.race <= 2) frame = RACE_COLOR[p.race];
                    break;
                }
            }
        }
        if (tag == null && last) { tag = S.t("neišsaugotas"); tagColor = Kit.LABEL; }
        final LinearLayout row = listRow(portrait, frame, rep.getName(),
                info != null ? replaySummary(info) : S.t("Kraunama…"), tag, tagColor, fileStamp(rep), rep.equals(selectedReplay));
        row.setOnClickListener(v -> selectReplayFile(rep));
        if (info == null) replayInfoAsync(rep, () -> {
            // fill this row in place (no list rebuild per file)
            if (row.getParent() == null) return;
            int at = ((ViewGroup) row.getParent()).indexOfChild(row);
            ViewGroup parent = (ViewGroup) row.getParent();
            parent.removeViewAt(at);
            parent.addView(replayCard(rep), at);
        });
        return row;
    }

    private void showReplayDetail(final File rep) {
        if (replayDetailBox == null) return;
        replayDetailBox.removeAllViews();
        TextView title = kit.text(rep.getName(), Color.WHITE, 20);
        title.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
        title.setPadding(0, 0, 0, dp(10));
        replayDetailBox.addView(title);
        ReplayInfo info = cachedReplayInfo(rep);
        if (info == null) {
            replayDetailBox.addView(kit.text(S.t("Kraunama..."), Kit.TEXT2, 13));
            replayInfoAsync(rep, () -> { if (rep.equals(selectedReplay)) { showReplayDetail(rep); updateStartButtonVisibility(); } });
            return;
        }
        if (info.error != null) {
            TextView e = kit.text(S.t("Antraštė nenuskaityta: ") + info.error, Kit.ACCENT, 13);
            replayDetailBox.addView(e);
        } else {
            replayDetailBox.addView(detailRow(S.t("Žemėlapis"), info.mapName.isEmpty() ? "?" : info.mapName));
            replayDetailBox.addView(detailRow(S.t("Trukmė"), info.duration()));
        }
        replayDetailBox.addView(detailRow(S.t("Data"), fileStamp(rep)));
        if (info.error == null) {
            replayDetailBox.addView(kit.label(S.t("Žaidėjai")));
            for (ReplayInfo.Player p : info.players) {
                LinearLayout row = new LinearLayout(this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setGravity(Gravity.CENTER_VERTICAL);
                row.setPadding(0, dp(5), 0, dp(5));
                // .rep stores names only: the race's default portrait stands in
                PortraitView pv = new PortraitView(this);
                pv.setScale(rowPortraitScale());
                pv.setBorderColor(p.race >= 0 && p.race <= 2 ? RACE_COLOR[p.race] : Kit.STEEL);
                pv.setPortrait(getExternalFilesDir(null), PortraitStore.aiPortrait(-1, p.race));
                row.addView(pv);
                LinearLayout who = new LinearLayout(this);
                who.setOrientation(LinearLayout.VERTICAL);
                who.setPadding(dp(12), 0, 0, 0);
                TextView n = kit.text(p.name, Color.WHITE, 15);
                n.setTypeface(android.graphics.Typeface.DEFAULT, android.graphics.Typeface.BOLD);
                who.addView(n);
                who.addView(kit.text(raceName(p.race) + (p.computer ? "  ·  AI" : ""), Kit.TEXT2, 12));
                row.addView(who);
                replayDetailBox.addView(row);
            }
            if (info.hasComputer) {
                TextView warn = kit.text(S.t("Žaidimas prieš kompiuterį — jo atkurti negalima."), Kit.DANGER, 12);
                warn.setPadding(0, dp(10), 0, 0);
                replayDetailBox.addView(warn);
            }
        }
    }

    private boolean replayPlayable(File rep) {
        ReplayInfo info = cachedReplayInfo(rep);
        return info != null && (info.error != null || !info.hasComputer);
    }

    private void confirmDeleteReplay(final File rep) {
        new AlertDialog.Builder(this)
                .setTitle(S.t("Pašalinti replay?"))
                .setMessage(rep.getName())
                .setPositiveButton(S.t("Pašalinti"), (d, w) -> {
                    if (rep.delete()) {
                        if (rep.equals(selectedReplay)) selectedReplay = null;
                        renderReplayList();
                        showReplayPlaceholder();
                    } else Toast.makeText(this, S.t("Nepavyko pašalinti."), Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }

    // Keep the last game's recording under a dated name (the same rule as the score screen).
    private void keepLastReplay(final File last) {
        String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT).format(new java.util.Date(last.lastModified()));
        File dst = new File(last.getParentFile(), stamp + ".rep");
        for (int k = 2; dst.exists() && k < 100; ++k) dst = new File(last.getParentFile(), stamp + "-" + k + ".rep");
        if (last.renameTo(dst)) {
            Toast.makeText(this, S.t("Išsaugota: ") + dst.getName(), Toast.LENGTH_LONG).show();
            selectReplayFile(dst);
        } else {
            Toast.makeText(this, S.t("Nepavyko išsaugoti replay."), Toast.LENGTH_LONG).show();
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        if (last == null) return null;
        int slash = last.lastIndexOf('/');
        return slash >= 0 ? last.substring(slash + 1) : last;
    }

    private void copyUri(Uri uri, File dst) throws Exception {
        File tmp = new File(dst.getParentFile(), dst.getName() + ".part");
        try (InputStream in = getContentResolver().openInputStream(uri);
             FileOutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
            try { out.getFD().sync(); } catch (Exception ignored) {}
        }
        if (dst.exists() && !dst.delete()) {
            tmp.delete();
            throw new Exception("nepavyko perrašyti esamo failo");
        }
        if (!tmp.renameTo(dst)) {
            tmp.delete();
            throw new Exception("nepavyko pervadinti laikinojo failo");
        }
    }

    private String localIp() {
        try {
            for (java.util.Enumeration<java.net.NetworkInterface> en = java.net.NetworkInterface.getNetworkInterfaces(); en.hasMoreElements();) {
                java.net.NetworkInterface ni = en.nextElement();
                for (java.util.Enumeration<java.net.InetAddress> a = ni.getInetAddresses(); a.hasMoreElements();) {
                    java.net.InetAddress addr = a.nextElement();
                    if (!addr.isLoopbackAddress() && addr instanceof java.net.Inet4Address) {
                        return addr.getHostAddress();
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return "?";
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    private int dp(int v) {
        return Math.round(getResources().getDisplayMetrics().density * v);
    }
}
