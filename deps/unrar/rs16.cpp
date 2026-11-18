#include "rar.hpp"

// We used "Screaming Fast Galois Field Arithmetic Using Intel SIMD
// Instructions" paper by James S. Plank, Kevin M. Greenan
// and Ethan L. Miller for fast SSE based multiplication.
// Also we are grateful to Artem Drobanov and Bulat Ziganshin
// for samples and ideas allowed to make Reed-Solomon codec more efficient.

RSCoder16::RSCoder16()
{
  Decoding=false;
  ND=NR=NE=0;
  ValidFlags=NULL;
  MX=NULL;

  gfInit();
}


RSCoder16::~RSCoder16()
{
  delete[] gfExp;
  delete[] gfLog;
  if (SharedMX==nullptr) // Unless using an external shared MX.
    delete[] MX;
  delete[] ValidFlags;
}


// Initialize logarithms and exponents Galois field tables.
void RSCoder16::gfInit()
{
  gfExp=new uint[4*gfSize+1];
  gfLog=new uint[gfSize+1];

  for (uint L=0,E=1;L<gfSize;L++)
  {
    gfLog[E]=L;
    gfExp[L]=E;
    gfExp[L+gfSize]=E;  // Duplicate the table to avoid gfExp overflow check.
    E<<=1;
    if (E>gfSize)
      E^=0x1100B; // Irreducible field-generator polynomial.
  }

  // log(0)+log(x) must be outside of usual log table, so we can set it
  // to 0 and avoid check for 0 in multiplication parameters.
  gfLog[0]= 2*gfSize;
  for (uint I=2*gfSize;I<=4*gfSize;I++) // Results for log(0)+log(x).
    gfExp[I]=0;
}


uint RSCoder16::gfAdd(uint a,uint b) // Addition in Galois field.
{
  return a^b;
}


uint RSCoder16::gfMul(uint a,uint b) // Multiplication in Galois field.
{
  return gfExp[gfLog[a]+gfLog[b]];
}


uint RSCoder16::gfInv(uint a) // Inverse element in Galois field.
{
  return a==0 ? 0:gfExp[gfSize-gfLog[a]];
}


bool RSCoder16::Init(uint DataCount, uint RecCount, bool *ValidityFlags)
{
  ND = DataCount;
  NR = RecCount;
  NE = 0;

  Decoding=ValidityFlags!=nullptr;
  if (Decoding)
  {
    delete[] ValidFlags;
    ValidFlags=new bool[ND + NR];

    for (uint I = 0; I < ND + NR; I++)
      ValidFlags[I]=ValidityFlags[I];
    for (uint I = 0; I < ND; I++)
      if (!ValidFlags[I])
        NE++;
    uint ValidECC=0;
    for (uint I = ND; I < ND + NR; I++)
      if (ValidFlags[I])
        ValidECC++;
    if (NE > ValidECC || NE == 0 || ValidECC == 0)
      return false;
  }

  // 2021.09.01 - we allowed RR and REV >100%, so no more NR > ND check.
  if (ND + NR > gfSize || /*NR > ND ||*/ ND == 0 || NR == 0)
    return false;

  // If external shared MX is already set, use it.
  if (SharedMX!=nullptr && *SharedMX!=nullptr)
    MX=*SharedMX;
  else
  {
    delete[] MX;
    if (Decoding)
    {
      MX=new uint[NE * ND];
      MakeDecoderMatrix();
      InvertDecoderMatrix();
    }
    else
    {
      MX=new uint[NR * ND];
      MakeEncoderMatrix();
    }
    if (SharedMX!=nullptr)
      *SharedMX=MX; // Assign to shared MX if it is defined.
  }
  return true;
}


void RSCoder16::MakeEncoderMatrix()
{
  // Create Cauchy encoder generator matrix. Skip trivial "1" diagonal rows,
  // which would just copy source data to destination.
  for (uint I = 0; I < NR; I++)
    for (uint J = 0; J < ND; J++)
      MX[I * ND + J] = gfInv( gfAdd( (I+ND), J) );
}


void RSCoder16::MakeDecoderMatrix()
{
  // Create Cauchy decoder matrix. Skip trivial rows matching valid data
  // units and containing "1" on main diagonal. Such rows would just copy
  // source data to destination and they have no real value for us.
  // Include rows only for broken data units and replace them by first
  // available valid recovery code rows.
  for (uint Flag=0, R=ND, Dest=0; Flag < ND; Flag++)
    if (!ValidFlags[Flag]) // For every broken data unit.
    {
      while (!ValidFlags[R]) // Find a valid recovery unit.
        R++;
      for (uint J = 0; J < ND; J++) // And place its row to matrix.
        MX[Dest*ND + J] = gfInv( gfAdd(R,J) );
      Dest++;
      R++;
    }
}


#ifdef RAR_SMP
THREAD_PROC(InvertThread)
{
  RSCoder16::InvertThreadData *td=(RSCoder16::InvertThreadData *)Data;
  td->Coder->InvertArea(td);
}


void RSCoder16::InvertArea(InvertThreadData *td)
{
  for (uint I = td->Start; I < td->End; I++)
    if (I != td->Kr) // For all rows except containing the pivot cell.
    {
      // Apply Gaussian elimination Mij -= Mkj * Mik / pivot.
      // Since pivot is already 1, it is reduced to Mij -= Mkj * Mik.
      uint *MXi = MX + I * ND; // i-th row of main matrix.
      uint *MIi = td->MI + I * ND; // i-th row of inversion matrix.
      uint Mik = MXi[td->Kf]; // Cell in pivot position.
      for (uint J = 0; J < ND; J++)
      {
        MXi[J] ^= gfMul(td->MXk[J] , Mik);
        MIi[J] ^= gfMul(td->MIk[J] , Mik);
      }
    }
}
#endif


// Apply Gauss-Jordan elimination to find inverse of decoder matrix.
// We have the square NDxND matrix, but we do not store its trivial
// diagonal "1" rows matching valid data, so we work with NExND matrix.
// Our original Cauchy matrix does not contain 0, so we skip search
// for non-zero pivot.
void RSCoder16::InvertDecoderMatrix()
{
  uint *MI=new uint[NE * ND]; // We'll create inverse matrix here.
  memset(MI, 0, ND * NE * sizeof(*MI)); // Initialize to identity matrix.
  for (uint Kr = 0, Kf = 0; Kr < NE; Kr++, Kf++)
  {
    while (ValidFlags[Kf]) // Skip trivial rows.
      Kf++;
    MI[Kr * ND + Kf] = 1;  // Set diagonal 1.
  }

  // Kr is the number of row in our actual reduced NE x ND matrix,
  // which does not contain trivial diagonal 1 rows.
  // Kf is the number of row in full ND x ND matrix with all trivial rows
  // included.
  for (uint Kr = 0, Kf = 0; Kf < ND; Kr++, Kf++) // Select pivot row.
  {
    // Matrix creation can take a lot of time for big NE * ND,
    // so provide interface feedback.
    if (NE * ND > 1024 * 1024)
      Wait();
    
    while (ValidFlags[Kf] && Kf < ND)
    {
      // Here we process trivial diagonal 1 rows matching valid data units.
      // Their processing can be simplified comparing to usual rows.
      // In full version of elimination we would set MX[I * ND + Kf] to zero
      // after MI[..]^=, but we do not need it for matrix inversion.
      for (uint I = 0; I < NE; I++)
        MI[I * ND + Kf] ^= MX[I * ND + Kf];
      Kf++;
    }

    if (Kf == ND)
      break;

    uint *MXk = MX + Kr * ND; // k-th row of main matrix.
    uint *MIk = MI + Kr * ND; // k-th row of inversion matrix.

    uint PInv = gfInv( MXk[Kf] ); // Pivot inverse.
    // Divide the pivot row by pivot, so pivot cell contains 1.
    for (uint I = 0; I < ND; I++)
    {
      MXk[I] = gfMul( MXk[I], PInv );
      MIk[I] = gfMul( MIk[I], PInv );
    }

    bool SMP=false;
#ifdef RAR_SMP
    if (RSThreadPool!=nullptr && MaxThreads>1 && NE>1024) // Use multithreaded code for large NE.
    {
      uint Threads=Min(MaxThreads,NE);
      InvertThreadData *ThreadData=new InvertThreadData[Threads];

      uint Step=NE/Threads;
      if (Step*Threads!=NE)
        Step++;
      for (uint Start=0,ThreadNum=0;Start<NE;Start+=Step,ThreadNum++)
      {
        InvertThreadData *td=ThreadData+ThreadNum;
        td->Coder=this;
        td->Start=Start;
        td->End=Min(Start+Step,NE);
        td->Kr=Kr;
        td->Kf=Kf;
        td->MI=MI;
        td->MXk=MXk;
        td->MIk=MIk;
        RSThreadPool->AddTask(InvertThread,(void*)td);
      }
      RSThreadPool->WaitDone();
      delete[] ThreadData;

      SMP=true;
    }
#endif

    if (!SMP) // Unless already built by multithreaded code.
      for (uint I = 0; I < NE; I++)
        if (I != Kr) // For all rows except containing the pivot cell.
        {
          // Apply Gaussian elimination Mij -= Mkj * Mik / pivot.
          // Since pivot is already 1, it is reduced to Mij -= Mkj * Mik.
          uint *MXi = MX + I * ND; // i-th row of main matrix.
          uint *MIi = MI + I * ND; // i-th row of inversion matrix.
          uint Mik = MXi[Kf]; // Cell in pivot position.
          for (uint J = 0; J < ND; J++)
          {
            MXi[J] ^= gfMul(MXk[J] , Mik);
            MIi[J] ^= gfMul(MIk[J] , Mik);
          }
        }
  }

  // Copy data to main matrix.
  for (uint I = 0; I < NE * ND; I++)
    MX[I] = MI[I];

  delete[] MI;
}


#if 0
// Multiply matrix to data vector. When encoding, it contains data in Data
// and stores error correction codes in Out. When decoding it contains
// broken data followed by ECC in Data and stores recovered data to Out.
// We do not use this function now, everything is moved to UpdateECC.
void RSCoder16::Process(const uint *Data, uint *Out)
{
  uint ProcData[gfSize];

  for (uint I = 0; I < ND; I++)
    ProcData[I]=Data[I];

  if (Decoding)
  {
    // Replace broken data units with first available valid recovery codes.
    // 'Data' array must contain recovery codes after data.
    for (uint I=0, R=ND, Dest=0; I < ND; I++)
      if (!ValidFlags[I]) // For every broken data unit.
      {
        while (!ValidFlags[R]) // Find a valid recovery unit.
          R++;
        ProcData[I]=Data[R];
        R++;
      }
  }

  uint H=Decoding ? NE : NR;
  for (uint I = 0; I < H; I++)
  {
    uint R = 0; // Result of matrix row multiplication to data.

    uint *MXi=MX + I * ND;
    for (uint J = 0; J < ND; J++)
      R ^= gfMul(MXi[J], ProcData[J]);

    Out[I] = R;
  }
}
#endif


// We write recovery data over former ECC blocks by applying every data block
// to each ECC block. This function applies one data block to one ECC block.
// Source data is guaranteed to be aligned to 16 bits. If vector commands
// are enabled, data is aligned to a value required for vector commands.
void RSCoder16::UpdateECC(uint DataNum, uint ECCNum, const byte *Data, byte *ECC, size_t BlockSize)
{
  // Init former ECC data before processing the first data block.
  // We use this former ECC as output buffer and we copied its actual
  // ECC contents over invalid data block before that.
  if (DataNum==0)
    memset(ECC, 0, BlockSize);

  bool DirectAccess;
#ifdef LITTLE_ENDIAN
  // We can access data and ECC directly if we have little endian 16 bit uint.
  DirectAccess=sizeof(ushort)==2;
#else
  DirectAccess=false;
#endif

#ifdef USE_SSE
  if (DirectAccess && SSE_UpdateECC(DataNum,ECCNum,Data,ECC,BlockSize))
    return;
#endif

#ifdef USE_NEON
  if (DirectAccess && NEON_UpdateECC(DataNum, ECCNum, Data, ECC, BlockSize))
    return;
#endif

  // For one Cauchy-matrix column coefficient M and one block of data, compute
  // ECC[i] ^= gfMul(M, Data[i]) for every 16-bit symbol i.
  uint M = MX[ECCNum * ND + DataNum];

  size_t Pos = 0;
#if defined(LITTLE_ENDIAN) && defined(ALLOW_MISALIGNED)
  if (DirectAccess)
  {
    // gfMul(M,D)=gfExp[gfLog[M]+gfLog[D]]. Precalculate gfLog[M].
    uint L = gfLog[M];

    // We need to apply gfMul( M, D[i] ) to every 16-bit 'D[i]' value in Data
    // and XOR result with matching 16-bit items in ECC. Since data sizes
    // here usually are much larger than 64K, it is efficient to precalculate
    // the multipliction table for all 64K values and use it when processing.
    // We also tried two 256 byte tables for low and high bytes, similarly to
    // what we do in SSE and Neon branches, but it was slower than 64K table.
    MultTab.resize(0x10000);
    for (uint I=0;I<MultTab.size();I++)
      MultTab[I] = gfExp[L+gfLog[I]];

    for (; Pos + 6 < BlockSize; Pos += 8) // Unroll loop.
    {
      uint64 D=*(const uint64 *)(Data + Pos); // Read 4 16-bit items at once.

      uint64 R = MultTab[uint16_t(D)];
      R |= uint64(MultTab[uint16_t(D>>16)])<<16;
      R |= uint64(MultTab[uint16_t(D>>32)])<<32;
      R |= uint64(MultTab[uint16_t(D>>48)])<<48;

      *(uint64 *)(ECC + Pos) ^= R; // Write 4 16-bit items at once.
    }

    for (; Pos < BlockSize; Pos += 2) // Process data tail.
    {
      ushort S = *(ushort *)(Data + Pos);
      *(ushort *)(ECC + Pos) ^= MultTab[S];
    }
    return;
  }
#endif

  for (; Pos<BlockSize; Pos+=2) // Unoptimized fallback code.
  {
    ushort S = RawGet2(Data+Pos); // Source 16-bit data item.
    ushort E = RawGet2(ECC+Pos); // 16-bit ECC item.
    ushort R = E ^ gfMul( M, S ); // Updated ECC item.
    RawPut2(R, ECC+Pos); // Store result.
  }
}


#ifdef USE_SSE
// We need to apply gfMul( M, D[i] ) to every 16-bit 'D[i]' value in Data.
// As we had seen in scalar code in UpdateECC(), it is faster to precalculate
// gfMul() results for all possible D[i] values.
//
// GF multiplication distributes over XOR. If we have 16-bit 0xhhll,
// were 'hh' is the high byte and 'll' is the low byte:
//     M * 0xhhll = (M * 0xhh00) XOR (M * 0x00ll)
// If we have MultTab[0x10000] containing all multiplication results
// for gfMul(M, i), where M is fixed and i is in 0 - 65535 range,
// we can get gfMul(M, 0xhhll) quickly by MultTab[0xhhll] lookup or as
// MultTab[0xhh00] ^ MultTab[0x00ll]. But in second case we use only 256 values
// in the table for high and low bytes. So we can use two 256 byte tables
// instead of one 64K. Low byte table stores gfMul(M, i) for i=0..255.
// High byte table stores 256 results of gfMul(M, i) for i with step 0x100,
// such as 0x0000, 0x0100, ..., 0xff00.
// Let's go further and split 16 bit D[i] to four 4-bit nibbles. We'll need
// to generate 4 tables for every nibble with 16 multiplication results
// in every table. We can fit 16 entries table into two 128-bit SSE registers,
// high bytes into one register and low bytes in another. We similarly split
// source data high and low bytes to different registers, so we can perform
// same table lookup for all 8 data bytes in register.
//
// Data and ECC addresses must be properly aligned for SSE.
// AVX2 did not provide a noticeable speed gain on i7-6700K here.
bool RSCoder16::SSE_UpdateECC(uint DataNum, uint ECCNum, const byte *Data, byte *ECC, size_t BlockSize)
{
  // Check data alignment and SSSE3 support.
  if ((size_t(Data) & (SSE_ALIGNMENT-1))!=0 || (size_t(ECC) & (SSE_ALIGNMENT-1))!=0 ||
      _SSE_Version<SSE_SSSE3)
    return false;

  uint M=MX[ECCNum * ND + DataNum];

  // Prepare tables containing products of M and 4, 8, 12, 16 bit length
  // numbers, which have 4 high bits in 0..15 range and other bits set to 0.
  // Store high and low bytes of resulting 16 bit product in separate tables.
  __m128i T0L,T1L,T2L,T3L; // Low byte tables.
  __m128i T0H,T1H,T2H,T3H; // High byte tables.

  for (uint I=0;I<16;I++)
  {
    ((byte *)&T0L)[I]=byte(gfMul(I,M));
    ((byte *)&T0H)[I]=byte(gfMul(I,M)>>8);
    ((byte *)&T1L)[I]=byte(gfMul(I<<4,M));
    ((byte *)&T1H)[I]=byte(gfMul(I<<4,M)>>8);
    ((byte *)&T2L)[I]=byte(gfMul(I<<8,M));
    ((byte *)&T2H)[I]=byte(gfMul(I<<8,M)>>8);
    ((byte *)&T3L)[I]=byte(gfMul(I<<12,M));
    ((byte *)&T3H)[I]=byte(gfMul(I<<12,M)>>8);
  }

  size_t Pos=0;

  __m128i LowByteMask=_mm_set1_epi16(0xff);     // 00ff00ff...00ff
  __m128i Low4Mask=_mm_set1_epi8(0xf);          // 0f0f0f0f...0f0f
  __m128i High4Mask=_mm_slli_epi16(Low4Mask,4); // f0f0f0f0...f0f0

  for (; Pos+2*sizeof(__m128i)<=BlockSize; Pos+=2*sizeof(__m128i))
  {
    // We process two 128 bit chunks of source data at once.
    __m128i *D=(__m128i *)(Data+Pos);

    // Place high bytes of both chunks to one variable and low bytes to
    // another, so we can use the table lookup multiplication for 16 values
    // 4 bit length each at once.
    __m128i HighBytes0=_mm_srli_epi16(D[0],8);
    __m128i LowBytes0=_mm_and_si128(D[0],LowByteMask);
    __m128i HighBytes1=_mm_srli_epi16(D[1],8);
    __m128i LowBytes1=_mm_and_si128(D[1],LowByteMask);
    __m128i HighBytes=_mm_packus_epi16(HighBytes0,HighBytes1);
    __m128i LowBytes=_mm_packus_epi16(LowBytes0,LowBytes1);

    __m128i LowBytesLow4=_mm_and_si128(LowBytes,Low4Mask);
    __m128i LowBytesMultSum=_mm_shuffle_epi8(T0L,LowBytesLow4);
    __m128i HighBytesMultSum=_mm_shuffle_epi8(T0H,LowBytesLow4);

    // Multiply bits 4..7 of low bytes. Store low and high product bytes separately.
    __m128i LowBytesHigh4=_mm_and_si128(LowBytes,High4Mask);
            LowBytesHigh4=_mm_srli_epi16(LowBytesHigh4,4);
    __m128i LowBytesHigh4MultLow=_mm_shuffle_epi8(T1L,LowBytesHigh4);
    __m128i LowBytesHigh4MultHigh=_mm_shuffle_epi8(T1H,LowBytesHigh4);

    // Add new product to existing sum, low and high bytes separately.
    LowBytesMultSum=_mm_xor_si128(LowBytesMultSum,LowBytesHigh4MultLow);
    HighBytesMultSum=_mm_xor_si128(HighBytesMultSum,LowBytesHigh4MultHigh);

    // Multiply bits 0..3 of high bytes. Store low and high product bytes separately.
    __m128i HighBytesLow4=_mm_and_si128(HighBytes,Low4Mask);
    __m128i HighBytesLow4MultLow=_mm_shuffle_epi8(T2L,HighBytesLow4);
    __m128i HighBytesLow4MultHigh=_mm_shuffle_epi8(T2H,HighBytesLow4);

    // Add new product to existing sum, low and high bytes separately.
    LowBytesMultSum=_mm_xor_si128(LowBytesMultSum,HighBytesLow4MultLow);
    HighBytesMultSum=_mm_xor_si128(HighBytesMultSum,HighBytesLow4MultHigh);

    // Multiply bits 4..7 of high bytes. Store low and high product bytes separately.
    __m128i HighBytesHigh4=_mm_and_si128(HighBytes,High4Mask);
            HighBytesHigh4=_mm_srli_epi16(HighBytesHigh4,4);
    __m128i HighBytesHigh4MultLow=_mm_shuffle_epi8(T3L,HighBytesHigh4);
    __m128i HighBytesHigh4MultHigh=_mm_shuffle_epi8(T3H,HighBytesHigh4);

    // Add new product to existing sum, low and high bytes separately.
    LowBytesMultSum=_mm_xor_si128(LowBytesMultSum,HighBytesHigh4MultLow);
    HighBytesMultSum=_mm_xor_si128(HighBytesMultSum,HighBytesHigh4MultHigh);

    // Combine separate low and high cumulative sum bytes to 16-bit words.
    __m128i HighBytesHigh4Mult0=_mm_unpacklo_epi8(LowBytesMultSum,HighBytesMultSum);
    __m128i HighBytesHigh4Mult1=_mm_unpackhi_epi8(LowBytesMultSum,HighBytesMultSum);

    // Add result to ECC.
    __m128i *StoreECC=(__m128i *)(ECC+Pos);

    StoreECC[0]=_mm_xor_si128(StoreECC[0],HighBytesHigh4Mult0);
    StoreECC[1]=_mm_xor_si128(StoreECC[1],HighBytesHigh4Mult1);
  }

  // If we have non 128 bit aligned data in the end of block, process them
  // in a usual way. We cannot do the same in the beginning of block,
  // because Data and ECC can have different alignment offsets.
  for (; Pos<BlockSize; Pos+=2)
    *(ushort*)(ECC+Pos) ^= gfMul( M, *(ushort*)(Data+Pos) );

  return true;
}
#endif


#ifdef USE_NEON
// NEON port of SSE_UpdateECC().
// See more detailed algorithm description in comment to SSE_UpdateECC().
// GF multiplication distributes over XOR. So:
// M * 0xabcd = (M * 0xa000) XOR (M * 0x0b00) XOR (M * 0x00c0) XOR (M * 0x000d)
// Like in SSE_UpdateECC(), we use 4 lookup tables for 4 bit nibbles,
// 16 entries in every table. Every table occupies 2 128-bit registers,
// storing low bytes of result in one register and high bytes in another.
bool RSCoder16::NEON_UpdateECC(uint DataNum, uint ECCNum,
                               const byte *Data, byte *ECC, size_t BlockSize)
{
  // Alignment guard. Might be excessive for NEON, but keep it for similarity
  // with SSE version. Also aligned access might be faster.
  if (((size_t)Data & (NEON_ALIGNMENT - 1)) != 0 || ((size_t)ECC & (NEON_ALIGNMENT - 1)) != 0)
    return false;

  // The Cauchy matrix coefficient for this (data column, ECC row) pair.
  uint M = MX[ECCNum * ND + DataNum];

  // Build four nibble tables, split into low byte and high byte halves.
  //
  // For index I (0..15) the "nibble value" we multiply by M is:
  //   T0: I          (nibble 0 = bits  0..3)
  //   T1: I << 4     (nibble 1 = bits  4..7)
  //   T2: I << 8     (nibble 2 = bits  8..11)
  //   T3: I << 12    (nibble 3 = bits 12..15)
  //
  // gfMul(M, value) returns a 16-bit GF product. We store its low byte in the
  // *L table and its high byte in the *H table, because NEON 8-bit tbl can
  // only index byte tables.
  //
  // alignas(16) is required: vqtbl1q_u8 reads a 16-byte table and vld1q_u8
  // loads must be 16-byte aligned on some toolchains.
  alignas(16) uint8_t T0L[16], T1L[16], T2L[16], T3L[16];
  alignas(16) uint8_t T0H[16], T1H[16], T2H[16], T3H[16];

  for (uint I = 0; I < 16; I++)
  {
    uint P;

    P = gfMul(I, M);
    T0L[I] = (uint8_t)P;            // low  byte of M*I
    T0H[I] = (uint8_t)(P >> 8);     // high byte of M*I

    P = gfMul(I << 4, M);
    T1L[I] = (uint8_t)P;
    T1H[I] = (uint8_t)(P >> 8);

    P = gfMul(I << 8, M);
    T2L[I] = (uint8_t)P;
    T2H[I] = (uint8_t)(P >> 8);

    P = gfMul(I << 12, M);
    T3L[I] = (uint8_t)P;
    T3H[I] = (uint8_t)(P >> 8);
  }

  // Load the byte tables into NEON registers once, outside the loop.
  // Each vqtbl1q_u8(tX?, idx) will look up idx (0..15) in the 16-byte table.
  uint8x16_t t0L = vld1q_u8(T0L);
  uint8x16_t t1L = vld1q_u8(T1L);
  uint8x16_t t2L = vld1q_u8(T2L);
  uint8x16_t t3L = vld1q_u8(T3L);

  uint8x16_t t0H = vld1q_u8(T0H);
  uint8x16_t t1H = vld1q_u8(T1H);
  uint8x16_t t2H = vld1q_u8(T2H);
  uint8x16_t t3H = vld1q_u8(T3H);

  // Mask to isolate bits 0 - 3.
  const uint8x16_t Low4Mask  = vdupq_n_u8(0x0f);

  size_t Pos = 0;

  // Main loop: process 32 bytes per iteration - 16 16-bit symbols.
  //
  // We deliberately work on two 16-byte source vectors (D0, D1) at once so
  // that the byte-extraction and byte-repacking steps produce full 128-bit
  // byte vectors (16 bytes = 16 symbols' worth of low/high bytes), matching
  // the SSE code's use of two __m128i data vectors and one packed byte vector.
  const size_t NEON_CHUNK = 32;

  for (; Pos + NEON_CHUNK <= BlockSize; Pos += NEON_CHUNK)
  {
    // Reinterpret the byte stream as 16-bit little-endian symbols.
    // This is valid because we checked alignment and we are on little-endian
    // (the caller's DirectAccess check guarantees this).
    const uint16_t *Src = (const uint16_t *)(Data + Pos);

    uint16x8_t D0 = vld1q_u16(Src);       // Load 16-bit symbols 0 - 7.
    uint16x8_t D1 = vld1q_u16(Src + 8);   // Load 16-bit symbols 8 - 15.

    // Shift each 16-bit value by 8 bits and keep one byte of result to
    // produce high bytes.
    uint8x8_t HighBytes0 = vshrn_n_u16(D0, 8);
    uint8x8_t HighBytes1 = vshrn_n_u16(D1, 8);

    // Convert each 16-bit value to 8 bits, keeping low bytes.
    uint8x8_t LowBytes0 = vmovn_u16(D0);
    uint8x8_t LowBytes1 = vmovn_u16(D1);

    // Combine two 64-bit vectors into 128-bit, producing 128-bit vectors
    // of source data high and low bytes.
    uint8x16_t HighBytes = vcombine_u8(HighBytes0, HighBytes1);
    uint8x16_t LowBytes  = vcombine_u8(LowBytes0, LowBytes1);

    // Bits 0 - 3 of low bytes.
    uint8x16_t LowBytesLow4  = vandq_u8(LowBytes, Low4Mask);
    // Bits 4 - 7 of low bytes.
    uint8x16_t LowBytesHigh4 = vshrq_n_u8(LowBytes, 4); // bits 4..7

    // Bits 0 - 3 of high bytes.
    uint8x16_t HighBytesLow4  = vandq_u8(HighBytes, Low4Mask);
    // Bits 4 - 7 of high bytes.
    uint8x16_t HighBytesHigh4 = vshrq_n_u8(HighBytes, 4);

    // vqtbl1q_u8 performs lookup in 128-bit multiplication table.
    // Multiply bits 0 - 3 of low bytes. Store low and high product bytes
    // separately in cumulative sum variables.
    uint8x16_t LowBytesMultSum  = vqtbl1q_u8(t0L, LowBytesLow4);
    uint8x16_t HighBytesMultSum = vqtbl1q_u8(t0H, LowBytesLow4);

    // Multiply bits 4 - 7 of low bytes and XOR result with partial product.
    LowBytesMultSum  = veorq_u8(LowBytesMultSum,  vqtbl1q_u8(t1L, LowBytesHigh4));
    HighBytesMultSum = veorq_u8(HighBytesMultSum, vqtbl1q_u8(t1H, LowBytesHigh4));

    // Multiply bits 0 - 3 of high bytes and XOR result with partial product.
    LowBytesMultSum  = veorq_u8(LowBytesMultSum,  vqtbl1q_u8(t2L, HighBytesLow4));
    HighBytesMultSum = veorq_u8(HighBytesMultSum, vqtbl1q_u8(t2H, HighBytesLow4));

    // Multiply bits 4 - 7 of high bytes and XOR result with partial product.
    LowBytesMultSum  = veorq_u8(LowBytesMultSum,  vqtbl1q_u8(t3L, HighBytesHigh4));
    HighBytesMultSum = veorq_u8(HighBytesMultSum, vqtbl1q_u8(t3H, HighBytesHigh4));

    // Now we have:
    //   LowBytesMultSum[i]  = low  byte of (M * symbol[i])
    //   HighBytesMultSum[i] = high byte of (M * symbol[i])
    // Convert them to two byte vectors with 16-bit little-endian words.
    uint8x16x2_t ProductBytes = vzipq_u8(LowBytesMultSum, HighBytesMultSum);

    // 16-bit multiplication product symbols 0  - 7.
    uint16x8_t Mult0 = vreinterpretq_u16_u8(ProductBytes.val[0]);
    // 16-bit multiplication product symbols 8  - 15.
    uint16x8_t Mult1 = vreinterpretq_u16_u8(ProductBytes.val[1]);

    uint16_t *Dst = (uint16_t *)(ECC + Pos);

    // Load ECC destination data.
    uint16x8_t E0 = vld1q_u16(Dst);
    uint16x8_t E1 = vld1q_u16(Dst + 8);

    // XOR ECC destination with multiplication product.
    E0 = veorq_u16(E0, Mult0);
    E1 = veorq_u16(E1, Mult1);

    // Store result.
    vst1q_u16(Dst, E0);
    vst1q_u16(Dst + 8, E1);
  }

  // If we have non 128 bit aligned data in the end of block, process them
  // in a usual way. We cannot do the same in the beginning of block,
  // because Data and ECC can have different alignment offsets.
  for (; Pos < BlockSize; Pos += 2)
    *(ushort *)(ECC + Pos) ^= gfMul(M, *(const ushort *)(Data + Pos));

  return true;
}
#endif
