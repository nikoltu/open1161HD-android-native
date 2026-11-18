#ifndef _RAR_RS16_
#define _RAR_RS16_

class RSCoder16
{
  public:
#ifdef RAR_SMP
    struct InvertThreadData
    {
      RSCoder16 *Coder;
      uint Start,End,Kr,Kf,*MI,*MXk,*MIk;
    };
    void InvertArea(InvertThreadData *td);
#endif
  private:
    static const uint gfSize=65535;   // Galois field size.
    void gfInit();                    // Galois field inititalization.
    inline uint gfAdd(uint a,uint b); // Addition in Galois field. 
    inline uint gfMul(uint a,uint b); // Multiplication in Galois field. 
    inline uint gfInv(uint a);        // Inverse element in Galois field.
    uint *gfExp;                      // Galois field exponents.
    uint *gfLog;                      // Galois field logarithms.

    std::vector<ushort> MultTab;      // Multiplication table.

    void MakeEncoderMatrix();
    void MakeDecoderMatrix();
    void InvertDecoderMatrix();

#ifdef USE_SSE
#ifdef __GNUC__
    __attribute__((target("ssse3")))
#endif
    bool SSE_UpdateECC(uint DataNum, uint ECCNum, const byte *Data, byte *ECC, size_t BlockSize);
#endif
#ifdef USE_NEON
    bool NEON_UpdateECC(uint DataNum, uint ECCNum, const byte *Data, byte *ECC, size_t BlockSize);
#endif

#ifdef RAR_SMP
    ThreadPool *RSThreadPool = nullptr;
    uint MaxThreads = 0;
#endif

    bool Decoding;    // If we are decoding or encoding data.
    uint ND;          // Number of data units.
    uint NR;          // Number of Reed-Solomon code units.
    uint NE;          // Number of erasures.
    bool *ValidFlags; // Validity flags for data and ECC units.
    uint *MX;         // Cauchy based coding or decoding matrix.

    // If we create multiple coders with same parameters for multithreaded
    // processing, it is faster and takes less memory to share a single MX copy
    // between coders. Especially noticeable when recoving a lot of RAR volumes
    // using REV volumes.
    uint **SharedMX = nullptr; // Points to pointer of optional external MX.
  public:
    RSCoder16();
    ~RSCoder16();

    bool Init(uint DataCount, uint RecCount, bool *ValidityFlags);
#if 0 // We use only UpdateECC now.
    void Process(const uint *Data, uint *Out);
#endif
    void UpdateECC(uint DataNum, uint ECCNum, const byte *Data, byte *ECC, size_t BlockSize);
#ifdef RAR_SMP
    void SetThreadPool(ThreadPool *Pool,uint Threads) {RSThreadPool=Pool;MaxThreads=Threads;}
#endif
    void SetSharedMX(uint **ExtMX) {SharedMX=ExtMX;}
};

#endif
