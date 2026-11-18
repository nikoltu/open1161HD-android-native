#include "rar.hpp"

static bool match(const wchar *pattern,const wchar *string,bool ForceCase);
static int mwcsicompc(const wchar *Str1,const wchar *Str2,bool ForceCase);
static int mwcsnicompc(const wchar *Str1,const wchar *Str2,size_t N,bool ForceCase);
static bool IsWildcard(const wchar *Str,size_t CheckSize);

inline uint touppercw(uint ch,bool ForceCase)
{
  if (ForceCase)
    return ch;
#if defined(_UNIX)
  return ch;
#else
  return toupperw(ch);
#endif
}


bool CmpName(const wchar *Wildcard,const wchar *Name,uint CmpMode)
{
  bool ForceCase=(CmpMode&MATCH_FORCECASESENSITIVE)!=0;

  CmpMode&=MATCH_MODEMASK;

  const wchar *Name1=PointToName(Wildcard);
  const wchar *Name2=PointToName(Name);

  if (CmpMode!=MATCH_NAMES)
  {
    size_t WildLength=wcslen(Wildcard);
    if (CmpMode!=MATCH_EXACT && CmpMode!=MATCH_EXACTPATH && CmpMode!=MATCH_ALLWILD &&
        mwcsnicompc(Wildcard,Name,WildLength,ForceCase)==0)
    {
      // For all modes except MATCH_NAMES, MATCH_EXACT, MATCH_EXACTPATH, MATCH_ALLWILD,
      // "path1" mask must match "path1\path2\filename.ext" and "path1" names.
      wchar NextCh=Name[WildLength];
      if (NextCh==L'\\' || NextCh==L'/' || NextCh==0)
        return true;
    }

    // Nothing more to compare for MATCH_SUBPATHONLY.
    if (CmpMode==MATCH_SUBPATHONLY)
      return false;
    
    // 2023.08.29: We tried std::wstring Path1 and Path2 here, but performance
    // impact for O(n^2) complexity loop in CmdExtract::AnalyzeArchive()
    // was rather noticeable, 1.7s instead of 0.9s when extracting ~300 files
    // with @listfile from archive with ~7000 files.
    // This function can be invoked from other O(n^2) loops. So for now
    // we prefer to avoid wstring and use pointers and path sizes here.
    // Another option could be using std::wstring_view.

    size_t Path1Size=Name1-Wildcard;
    size_t Path2Size=Name2-Name;

    if ((CmpMode==MATCH_EXACT || CmpMode==MATCH_EXACTPATH) && 
        (Path1Size!=Path2Size ||
        mwcsnicompc(Wildcard,Name,Path1Size,ForceCase)!=0))
      return false;
    if (CmpMode==MATCH_ALLWILD)
      return match(Wildcard,Name,ForceCase);
    if (CmpMode==MATCH_SUBPATH || CmpMode==MATCH_WILDSUBPATH)
      if (IsWildcard(Wildcard,Path1Size))
        return match(Wildcard,Name,ForceCase);
      else
        if (CmpMode==MATCH_SUBPATH || IsWildcard(Wildcard))
        {
          if (Path1Size>0 && mwcsnicompc(Wildcard,Name,Path1Size,ForceCase)!=0)
            return false;
        }
        else
          if (Path1Size!=Path2Size || mwcsnicompc(Wildcard,Name,Path1Size,ForceCase)!=0)
            return false;
  }

  if (CmpMode==MATCH_EXACT)
    return mwcsicompc(Name1,Name2,ForceCase)==0;

  return match(Name1,Name2,ForceCase);
}


// 2026.09.23: Updated to faster non-recursive version.
bool match(const wchar *pattern, const wchar *string, bool ForceCase)
{
  const wchar *star = nullptr; // Points to the last '*' seen.
  const wchar *last_s = nullptr; // String position checked after last '*'

  while (*string != 0)
  {
    wchar stringc = touppercw(*string, ForceCase);
    wchar patternc = touppercw(*pattern, ForceCase);

    if (stringc == patternc || *pattern == '?')
    {
      // String character matched pattern character or '?'.
      // Advance both pointers.
      string++;
      pattern++;
      continue;
    }
    if (*pattern == '*')
    {
      if (pattern[1] == '.' && pattern[2] == 0)
      {
        // Allow file*. to match both 'file' and 'file.', but not 'file.ext'.
        const wchar *dot=wcschr(string,'.');
        return dot==nullptr || dot[1]==0;
      }
      
      // Found '*' in pattern. Save position after '*' in pattern,
      // we'll compare it against all positions of remaining part of string.
      star = ++pattern;

      // Store the first position to check in string in last_s,
      // we'll be incrementing it.
      last_s = string;
      continue;
    }
    if (star) // Current characters mismatched, but we had '*' in pattern.
    {
      pattern = star; // Return pattern to first position after '*'.
      string = ++last_s; // Increment the string position to check.

      continue;
    }

    return false; // No '*' and characters mismatch.
  }

  // String is over, skip remaining trailing asterisks in the pattern.
  while (*pattern == '*')
    pattern++;

  // Also match 'file' with 'file.*'.
  if (pattern[0]=='.' && pattern[1]=='*' && pattern[2]==0)
    return true;

  return *pattern==0; // Match if nothing left in pattern.
}


int mwcsicompc(const wchar *Str1,const wchar *Str2,bool ForceCase)
{
  if (ForceCase)
    return wcscmp(Str1,Str2);
  return wcsicompc(Str1,Str2);
}


int mwcsnicompc(const wchar *Str1,const wchar *Str2,size_t N,bool ForceCase)
{
  if (ForceCase)
    return wcsncmp(Str1,Str2,N);
#if defined(_UNIX)
  return wcsncmp(Str1,Str2,N);
#else
  return wcsnicomp(Str1,Str2,N);
#endif
}


bool IsWildcard(const wchar *Str,size_t CheckSize)
{
  size_t CheckPos=0;
#ifdef _WIN_ALL
  // Not treat the special NTFS \\?\d: path prefix as a wildcard.
  if (Str[0]=='\\' && Str[1]=='\\' && Str[2]=='?' && Str[3]=='\\')
    CheckPos+=4;
#endif
  for (size_t I=CheckPos;I<CheckSize && Str[I]!=0;I++)
    if (Str[I]=='*' || Str[I]=='?')
      return true;
  return false;
}
