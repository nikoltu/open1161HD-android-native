void CommandData::OutTitle()
{
  // If -idc is present in configuration, -iver shall override it.
  if (BareOutput || DisableCopyright && !PrintVersion)
    return;
#ifdef SFX_MODULE
  mprintf(St(MCopyrightS));
#else
#ifndef SILENT
  static bool TitleShown=false;
  if (TitleShown)
    return;
  TitleShown=true;

  std::wstring Version=wstrprintf(L"%d.%02d",RARVER_MAJOR,RARVER_MINOR);
  if (RARVER_BETA!=0)
    Version+=wstrprintf(L" %ls %u",St(MBeta),RARVER_BETA);
#ifdef _WIN_ARSTR
  Version+=L" " _WIN_ARSTR; // Combine strings in preprocessor.
#endif

  if (PrintVersion)
  {
    mprintf(L"%s\n",Version.c_str());
    exit(0);
  }
  mprintf(St(MUCopyright),Version.c_str(),RARVER_YEAR);
#endif
#endif
}


inline bool CmpMSGID(MSGID i1,MSGID i2)
{
#ifdef MSGID_INT
  return i1==i2;
#else
  // If MSGID is const char*, we cannot compare pointers only.
  // Pointers to different instances of same string can differ,
  // so we need to compare complete strings.
  return wcscmp(i1,i2)==0;
#endif
}


void CommandData::OutHelp(RAR_EXIT ExitCode)
{
#if !defined(SILENT)
  OutTitle();
  static MSGID Help[]={
#ifdef SFX_MODULE
    // Console SFX switches definition.
    MCHelpCmd,MSHelpCmdE,MSHelpCmdT,MSHelpCmdV
#else
    // UnRAR switches definition.
    MUNRARTitle1,MRARTitle2,MCHelpCmd,MCHelpCmdE,MCHelpCmdL,
    MCHelpCmdP,MCHelpCmdT,MCHelpCmdV,MCHelpCmdX,MCHelpSw,MCHelpSwm,
    MCHelpSwAT,MCHelpSwAC,MCHelpSwAD,MCHelpSwAG,MCHelpSwAI,MCHelpSwAP,
    MCHelpSwCm,MCHelpSwCFGm,MCHelpSwCL,MCHelpSwCU,MCHelpSwDA,MCHelpSwDH,
    MCHelpSwEP,MCHelpSwEP3,MCHelpSwEP4,MCHelpSwF,MCHelpSwIDP,MCHelpSwIERR,
    MCHelpSwINUL,MCHelpSwIOFF,MCHelpSwKB,MCHelpSwLim,MCHelpSwME,MCHelpSwMLP,
    MCHelpSwN,MCHelpSwNa,MCHelpSwNal,MCHelpSwO,MCHelpSwOC,MCHelpSwOL,
    MCHelpSwOM,MCHelpSwOP,MCHelpSwOR,MCHelpSwOW,MCHelpSwP,MCHelpSwR,
    MCHelpSwRI,MCHelpSwSC,MCHelpSwSI,MCHelpSwSL,MCHelpSwTA,MCHelpSwTB,
    MCHelpSwTN,MCHelpSwTO,MCHelpSwTS,MCHelpSwU,MCHelpSwVUnr,MCHelpSwVER,
    MCHelpSwVP,MCHelpSwX,MCHelpSwXa,MCHelpSwXal,MCHelpSwY
#endif
  };

  for (uint I=0;I<ASIZE(Help);I++)
  {
#ifndef SFX_MODULE
#ifndef _WIN_ALL
    static MSGID Win32Only[]={
      MCHelpSwIEML,MCHelpSwVD,MCHelpSwAO,MCHelpSwOS,MCHelpSwIOFF,
      MCHelpSwEP2,MCHelpSwMLP,MCHelpSwOC,MCHelpSwONI,MCHelpSwDR,MCHelpSwRI
    };
    bool Found=false;
    for (uint J=0;J<ASIZE(Win32Only);J++)
      if (CmpMSGID(Help[I],Win32Only[J]))
      {
        Found=true;
        break;
      }
    if (Found)
      continue;
#endif
#ifdef _UNIX
    if (CmpMSGID(Help[I],MRARTitle2))
    {
      mprintf(St(MFwrSlTitle2));
      continue;
    }
#endif
#if !defined(_UNIX) && !defined(_WIN_ALL)
    if (CmpMSGID(Help[I],MCHelpSwOW))
      continue;
#endif
#ifndef _WIN_ALL
    if (CmpMSGID(Help[I],MCHelpSwAC))
      continue;
#endif
#ifndef RAR_SMP
    if (CmpMSGID(Help[I],MCHelpSwMT))
      continue;
#endif
#endif
    mprintf(St(Help[I]));
  }
  mprintf(L"\n");
  ErrHandler.Exit(ExitCode);
#endif
}

