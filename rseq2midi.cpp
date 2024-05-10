/**************************************/
/* rseq2midi - RSEQ Conversion Tool   */
/* Copyright (C) 2010-11, Ruben Nunez */
/**************************************/
/* Changelog:                         */
/*   16-09-01  -Valley Bell           */
/*     write mod depth as MIDI ctrl 1 */
/*     turn debug ctrls into option   */
/*     detect jump direction          */
/*   16-04-16  -Valley Bell           */
/*     added LABL support             */
/*     stop all notes on track end    */
/*     add ignoreJump option          */
/*     add debug MIDI controllers     */
/*   11-11-01                         */
/*     Revamped code.                 */
/*     Fixed up delta timing issue.   */
/*     Magic ID -> big endian.        */
/*     Enhanced readability.          */
/*     Slight optimizations.          */
/*   10-07-08                         */
/*     Original version.              */
/**************************************/
/* Notes:                             */
/*   Command list:                    */
/*     80 - Wait [var. length arg]    */
/*     81 - Program [1-3 args]        */
/*     88 - Split [track, offset]     */
/*     89 - Jump [offset]             */
/*     8A - Call [offset]             */
/*     B0 - Unknown                   */
/*     C0 - Pan [0.127]               */
/*     C1 - Volume [0.127]            */
/*     C2 - Master volume [0.127]     */
/*     C3 - Transpose [-128.+127]     */
/*     C4 - Bend [-128.+127]          */
/*     C5 - Bend range [0.127]        */
/*     C6 - Priority [0.?]            */
/*     C7 - Polyphony [?]             */
/*     C8 - Tie ???                   */
/*     C9 - Portamento control [?]    */
/*     CA - Mod depth [0.127]         */
/*     CB - Mod speed [0.127]         */
/*     CC - Mod type [?]              */
/*     CD - Mod range [?]             */
/*     CE - Portamento [?]            */
/*     CF - Portamento time [?]       */
/*     D0 - Attack [0.127]            */
/*     D1 - Decay [0.127]             */
/*     D2 - Sustain [0.127]           */
/*     D3 - Release [0.127]           */
/*     D4 - Loop start [marker?]      */
/*     D5 - Expression [0.127]        */
/*     D6 - Print???                  */
/*     D8 - ???                       */
/*     D9 - ???                       */
/*     DA - ???                       */
/*     DB - ???                       */
/*     E0 - Mod delay [?]             */
/*     E1 - Tempo [0.65535]           */
/*     E3 - Sweep?                    */
/*     FC - Loop end [marker?]        */
/*     FD - Return                    */
/*     FE - Track usage [16-bit]      */
/*     FF - Fine                      */
/**************************************/
#define DEBUG
/**************************************/
#define CHNK_HAVE_DATA (0x01)
#define CHNK_HAVE_LABL (0x02)
#define CHNK_NEEDED    (CHNK_HAVE_DATA)
/**************************************/
#define BOOL_EQUAL(x,y) (((x)&(y)) == (y))
/**************************************/
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf	_snprintf	// use _snprintf for Visual Studio 2013 and earlier
#endif
/**************************************/
using namespace std;
/**************************************/

typedef   signed char      s8 ;
typedef unsigned char      u8 ;
typedef   signed short     s16;
typedef unsigned short     u16;
typedef   signed int       s32;
typedef unsigned int       u32;

/**************************************/
bool ignoreJumps = false;
bool debugCtrls = false;
/**************************************/

typedef struct {
	u32 key; //! key
	u32 pos; //! end [tick]
} Note_t;

/**************************************/

static inline u32 Swap32(u32 x) {
	return (x>>24) | 
	       ((x<<8) & 0x00FF0000) |
	       ((x>>8) & 0x0000FF00) |
	       (x<<24);
}

static inline u16 Swap16(u16 x) {
	return (x>>8) | (x<<8);
}

/**************************************/

static inline u32 ReadLE(FILE *f, u32 b) {
	u32 v = 0;
	for(u32 i=0;i<b;i+=8) v |= fgetc(f) << i;
	return v;
}

static inline u32 ReadBE(FILE *f, s32 b) {
	u32 v = 0;
	for(s32 i=b-8;i>=0;i-=8) v |= fgetc(f) << i;
	return v;
}

/**************************************/

static inline u32 ReadVarLen(FILE *f) {
	u32 t = 0;
	while(1) {
		u32 c = fgetc(f);
		t = (t<<7) | (c&127);
		
		if((c&0x80) == 0) break;
	} return t;
}

/**************************************/

static inline void DebugMsg(const char *str, ...) {
#ifdef DEBUG
	static FILE *dstF = fopen("rseq2midi.log.txt", "wt");
	
	fseek(dstF, 0, SEEK_END);
	va_list myList;
	va_start(myList, str);
	vfprintf(dstF, str, myList);
	va_end(myList);
#endif
}

/**************************************/

//! compare function for sorting note order
static int NoteSortCmp(const void *a, const void *b) {
	return ((Note_t*)a)->pos - ((Note_t*)b)->pos;
}

/**************************************/

typedef struct {
	u8             gIndx; //! self-index
	u8             gStat; //! on/off
	s8             gTrns; //! transpose
	u8             gRPNR; //! RPNs ready
	u32            gWait; //! waiting left
	u32            gDPos; //! data position [offset]
	u32            gGPos; //! global position [tick]
	u32            gRPos; //! return position [offset]
	vector<Note_t> gNote; //! notes
	vector<u8>     gData; //! midi data
	
	//! reset track
	void Reset(u32 idx) {
		gIndx = idx;
		gStat = 0;
		gTrns = 0;
		gRPNR = 0;
		gDPos = 0;
		gGPos = 0;
		gRPos = 0;
		gNote.clear();
		gData.clear();
	}
	
	//! start track
	void Start(u32 adr) {
		//! init struct data
		gStat = 1;
		gTrns = 0;
		gDPos = adr;
		gGPos = 0;
		gRPos = 0;
		gData.clear();
		gNote.clear();
		
		//! debug stuff
		DebugMsg("  Trk %02u started from 0x%X...\n", gIndx, adr);
	}
	
	//! write midi-style delta
	void PushDelta(u32 t) {
		s32 c = 0;
		u32 n = t;
		
		//! count bytes required
		while(n > 127) {
			c++;
			n >>= 7;
		}
		
		//! write individual data
		for(int i=c;i>=0;i--) {
			//! calculate value
			u32 v = (t>>(7*i)) & 127;
			if(i) v |= 0x80;
			
			//! push data to seq
			gData.push_back(v);
		}
	}
	
	//! process delta
	void ProcDelta(void) {
		PushDelta(gWait);
		gWait = 0;
	}
	
	//! write event to seq
	void Event(u32 ev, u32 argc, const u8 *argv) {
		//! process delta
		ProcDelta();
		
		//! push back command + arguments
		gData.push_back(ev|gIndx);
		while(argc--) gData.push_back(*argv++);
	}
	
	//! note-on
	void mNoteOn(u32 key, u32 vel, u32 time) {
		//! write data
		//Event(0x90, 2, (const u8[]) {key, vel});
		const u8 edata[] = {key, vel};
		Event(0x90, 2, edata);
		
		//! push note into stack
		Note_t note = {key, gGPos + time};
		gNote.push_back(note);
	}
	
	//! set panning
	void mGenCtrl(u8 ctrlType, u8 ctrlData) {
		//! write data
		const u8 edata[] = {ctrlType, ctrlData};
		Event(0xB0, 2, edata);
	}
	
	//! set volume
	void mVol(u32 vol) {
		//! write data
		//Event(0xB0, 2, (const u8[]) {0x07, vol});
		const u8 edata[] = {0x07, vol};
		Event(0xB0, 2, edata);
	}
	
	//! set panning
	void mPan(u32 pan) {
		//! write data
		//Event(0xB0, 2, (const u8[]) {0x0A, pan});
		const u8 edata[] = {0x0A, pan};
		Event(0xB0, 2, edata);
	}
	
	//! set expression
	void mExp(u32 exp) {
		//! write data
		//Event(0xB0, 2, (const u8[]) {0x0B, exp});
		const u8 edata[] = {0x0B, exp};
		Event(0xB0, 2, edata);
	}
	
	//! set program
	void mPrg(u32 prg) {
		//! write data
		//Event(0xC0, 1, (const u8[]) {prg});
		const u8 edata[] = {prg};
		Event(0xC0, 1, edata);
	}
	
	//! set bend amount
	void mBnd(u32 bnd) {
		//! scale 
		u32 n = 0x2000 + bnd*16384/256;
		
		//! write data
		//Event(0xE0, 2, (const u8[]) {n&127, n>>7});
		const u8 edata[] = {n&127, n>>7};
		Event(0xE0, 2, edata);
	}
	
	//! set bend range
	void mBndRng(u32 rng) {
		//! RPNs ready?
		if(!gRPNR) {
			//! no, write them
			gRPNR = 1;
			//Event(0xB0, 2, (const u8[]) {0x64, 0}); //! low
			//Event(0xB0, 2, (const u8[]) {0x65, 0}); //! high
			const u8 edata2[] = {0x65, 0};
			const u8 edata1[] = {0x64, 0};
			Event(0xB0, 2, edata2); //! high
			Event(0xB0, 2, edata1); //! low
		}
		
		//! write data
		//Event(0xB0, 2, (const u8[]) {0x06, rng});
		const u8 edata[] = {0x06, rng};
		Event(0xB0, 2, edata);
	}
	
	//! write RPN controller
	void mRPN(u8 msb, u8 lsb, u32 data) {
		const u8 edata2[] = {0x65, lsb};
		const u8 edata1[] = {0x64, msb};
		Event(0xB0, 2, edata2); //! high
		Event(0xB0, 2, edata1); //! low
		
		//! write data
		const u8 edata[] = {0x06, data};
		Event(0xB0, 2, edata);
		
		gRPNR = 0;
	}
	
	//! write NRPN controller
	void mNRPN(u8 msb, u8 lsb, u32 data) {
		const u8 edata2[] = {0x63, lsb};
		const u8 edata1[] = {0x62, msb};
		Event(0xB0, 2, edata2); //! high
		Event(0xB0, 2, edata1); //! low
		
		//! write data
		const u8 edata[] = {0x06, data};
		Event(0xB0, 2, edata);
		
		gRPNR = 0;
	}
	
	//! set tempo
	void mTmp(u32 tmp) {
		//! calculate ms per quarter note
		u32 n = 60000000 / tmp;
		
		//! write data
		//Event(0xFF, 5, (const u8[]) {0x51, 3, n>>16, n>>8, n>>0});
		const u8 edata[] = {0x51, 3, n>>16, n>>8, n>>0};
		Event(0xFF, 5, edata);
	}
	
	//! set tempo
	void mMetaEvent(u8 type, u32 len, const u8* data) {
		//! write data
		Event(0xFF, 1, &type);
		PushDelta(len);
		while(len--) gData.push_back(*data++);
	}
	
	//! kill track
	void mEnd(void) {
		//! flush all running notes
		for(u32 i=0;i<gNote.size();i++) {
			//! fetch pointer to note
			Note_t &note = gNote[i];
			
			//! push delta
			PushDelta(gWait);
			gWait = 0;
			
			//! push note-off
			gData.push_back(0x90|gIndx);
			gData.push_back(note.key  );
			gData.push_back(0         );
		}
		gNote.clear();
		
		//! send kill command
		//Event(0xFF, 2, (const u8[]) {0x2F,0});
		const u8 edata[] = {0x2F,0};
		Event(0xFF, 2, edata);
		
		//! turn off
		gStat = 0;
	}
	
	//! wait n ticks
	void Wait(u32 timeLeft) {
		//! sort notes
		if(u32 len = gNote.size()) qsort(&gNote[0], len, sizeof(Note_t), NoteSortCmp);
		
		//! final position after this
		u32 pPos = gGPos + timeLeft;
		
		//! process notes from front
		for(u32 i=0;i<gNote.size();i++) {
			//! fetch pointer to note
			Note_t &note = gNote[i];
			
			//! doesn't end this tick?
			if(note.pos > pPos) continue;
			
			//! take time until note ends
			u32 dif = note.pos - gGPos;
			
			//! push a delta that long
			PushDelta(dif);
			
			//! push note-off
			gData.push_back(0x90|gIndx);
			gData.push_back(note.key  );
			gData.push_back(0         );
			
			//! destroy note
			gNote.erase(gNote.begin() + i);
			i--;
			
			//! set new position
			gGPos    += dif;
			timeLeft -= dif;
		}
		
		//! set new position
		gGPos += timeLeft;
		gWait += timeLeft;
	}
} Track_t;

/**************************************/

typedef struct {
	u32 id;     //! chunk ID
	u32 magic;  //! magic [0xFEFF0100]
	u32 size;   //! chunk length
	u16 cSize;  //! chunk header size
	u16 cBlock; //! chunk blocks
	u32 reserved[4];
} RSEQHead_t;

/**************************************/

typedef struct {
	u32 id;     //! chunk ID
	u32 size;   //! chunk length
	u32 offset; //! seq data offset
	
	u32 fOff;   //! first track offset
} DATAHead_t;

/**************************************/

typedef struct {
	u32 id;     //! chunk ID
	u32 size;   //! chunk length
	u32 labels; //! number of labels
	
	u32 lOff;   //! label base offset
} LABLHead_t;

/**************************************/

typedef std::map<u32, std::string> rseq_label_t;

/**************************************/

static struct {
	//! state
	u32 gStat;
	
	//! RSEQ chunk header
	RSEQHead_t gRSEQHead;
	
	//! DATA chunk header
	DATAHead_t gDATAHead;
	
	//! DATA chunk header
	LABLHead_t gLABLHead;
	
	//! RSEQ tracks
	Track_t gTrack[16];
	
	//! Label Data
	rseq_label_t gLabels;
	
	//! reset all
	void Reset(void) {
		//! clear state
		gStat = 0;
		
		//! clear chunk headers
		memset(&gRSEQHead, 0, sizeof(gRSEQHead));
		memset(&gDATAHead, 0, sizeof(gDATAHead));
		memset(&gLABLHead, 0, sizeof(gLABLHead));
		gLabels.clear();
		
		//! reset tracks
		for(int i=0;i<16;i++) gTrack[i].Reset(i);
	}
} gData;

/**************************************/

void rseqDo(FILE *midi, FILE *rseq) {
	u32 mdOff = gData.gDATAHead.fOff;
	
	//! debug
	DebugMsg("  Begin decoding...\n");
	
	//! start track 0
	gData.gTrack[0].Start(mdOff);
	
	//! process while there's tracks
	//! this setup is needed 'just in case'
	//! as tracks can spawn from *any* track
	bool gTrkCnt = true;
	while(gTrkCnt) {
		//! flip process flag
		gTrkCnt = false;
		
		//! process each track
		for(u32 i=0;i<16;i++) {
			//! verify track is active
			Track_t *trk = &gData.gTrack[i];
			if(!trk->gStat) continue;
			
			//! continue the main loop as we have a track
			gTrkCnt = true;
			
			//! seek to current track position
			fseek(rseq, trk->gDPos, SEEK_SET);
			
			//! loop until end of track
			bool loop = true;
			u32 lcount = 0;
			while(loop) {
				u32 curpos = ftell(rseq) - mdOff;
				rseq_label_t::iterator it = gData.gLabels.find(curpos);
				if (it != gData.gLabels.end())
				{
					std::string& data = it->second;
					// Write Event FF 06 data.length(), data.c_str();
					trk->mMetaEvent(0x06, data.length(), (u8*)data.c_str());
				}
				
				//! note on [implicit command]
				u32 cmd = fgetc(rseq);
				u32 cdata;
				if(cmd < 0x80) {
					//! read data
					u32 key = cmd;
					u32 vel = fgetc(rseq);
					u32 len = ReadVarLen(rseq);
					
					//! push note-on
					trk->mNoteOn(key, vel, len);
					
					//! continue loop
					continue;
				}
				
				//! switch command
				switch(cmd) {
					//! rest
					case 0x80: {
						//! read time
						u32 len = ReadVarLen(rseq);
						trk->Wait(len);
					} break;
					
					//! program:bank
					case 0x81: {
						//! fetch tone
						u32 c = fgetc(rseq);
						trk->mPrg(c&127);
						
						//! read/skip bank select command if needed
						if(c&0x80) c = fgetc(rseq);
						if(c&0x80) c = fgetc(rseq);
					} break;
					
					//! split
					case 0x88: {
						//! fetch info
						u32 trk = ReadBE(rseq,  8);
						u32 adr = ReadBE(rseq, 24) + mdOff;
						
						//! start new track
						gData.gTrack[trk].Start(adr);
					} break;
					
					//! jump
					case 0x89: {
						//! fetch address
						u32 adr = ReadBE(rseq, 24) + mdOff;
						
						char msgbuf[0x20];
						const char* jumpDirMsg;
						const char* jumpMsg;
						bool jumpDir;
						bool takeJump = false;
						
						jumpDir = (adr > ftell(rseq));
						jumpDirMsg = jumpDir ? "forwards" : "backwards";
						if (jumpDir)
							takeJump = true;
						else
						{
							//lcount ++;
							//if (lcount < 2)
							//	takeJump = true;
						}
						
						if (ignoreJumps)
							jumpMsg = "ignored";
						else if (takeJump)
							jumpMsg = "taken";
						else
							jumpMsg = "Track End";
						
						//! debug stuff
						DebugMsg("  Trk %02u: Jump (%s) to 0x%X\n", i, jumpDirMsg, adr);
						
						snprintf(msgbuf, 0x20, "Jump (%s, %s)", jumpDirMsg, jumpMsg);
						trk->mMetaEvent(0x06, strlen(msgbuf), (u8*)msgbuf);
						
						if (! ignoreJumps)
						{
							if (takeJump)
							{
								//! take forward jump: jump to + set new address
								fseek(rseq, trk->gDPos = adr, SEEK_SET);
							}
							else
							{
								//! kill track, stop read loop
								trk->mEnd();
								loop = false;
							}
						}
					} break;
					
					//! call
					case 0x8A: {
						//! fetch targe address
						u32 adr = mdOff + ReadBE(rseq, 24);
						
						//! set return address
						trk->gRPos = ftell(rseq);
						
						//! debug stuff
						DebugMsg("  Trk %02u: Call to 0x%X\n", i, adr);
						
						//! jump to + set new address
						fseek(rseq, trk->gDPos = adr, SEEK_SET);
					} break;
					
					//! unknown - 1 byte?
					case 0xB0: {
						//! skip argument
						//fseek(rseq, 1, SEEK_CUR);
						cdata = fgetc(rseq);
						if (debugCtrls)
						{
							trk->mGenCtrl(0x70, cmd & 0x7F);
							trk->mGenCtrl(0x26, cdata);
						}
					} break;
					
					//! pan
					case 0xC0: {
						//! set pan
						trk->mPan(fgetc(rseq));
					} break;
					
					//! volume
					case 0xC1: {
						//! set volume
						trk->mVol(fgetc(rseq));
					} break;
					
					//! master vol
					case 0xC2: {
						//! just read argument
						//! unsure on how to handle this, tbh
						//! maybe vol*mvol/127 on every call?
						cdata = fgetc(rseq);
						trk->mGenCtrl(0x27, cdata);
					} break;
					
					//! transpose
					case 0xC3: {
						//! step amount
						//trk->mTranspose(fgetc(rseq));
						cdata = fgetc(rseq);
						trk->mNRPN(0x00, 0x02, cdata);
					} break;
					
					//! bend
					case 0xC4: {
						//! bend
						trk->mBnd(fgetc(rseq));
					} break;
					
					//! bend range
					case 0xC5: {
						//! bend range
						trk->mBndRng(fgetc(rseq));
					} break;
					
					//! priority
					case 0xC6: {
						//! just read argument
						//! AFAIK, has no meaning in midi
						cdata = fgetc(rseq);
						if (debugCtrls)
						{
							trk->mGenCtrl(0x70, cmd & 0x7F);
							trk->mGenCtrl(0x26, cdata);
						}
					} break;
					
					//! polyphony
					case 0xC7: {
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
						{
							trk->mGenCtrl(0x70, cmd & 0x7F);
							trk->mGenCtrl(0x26, cdata);
						}
					} break;
					
					//! tie ???
					case 0xC8: {
						cdata = fgetc(rseq);
						if (debugCtrls)
						{
							trk->mGenCtrl(0x70, cmd & 0x7F);
							trk->mGenCtrl(0x26, cdata);
						}
					} break;
					
					//! portamento cnt
					case 0xC9: {
						//! not bothering with this
						cdata = fgetc(rseq);
						trk->mGenCtrl(84, cdata);
					} break;
					
					//! mod-depth
					case 0xCA: {
						//! not bothering with this
						cdata = fgetc(rseq);
						trk->mGenCtrl(1, cdata);
					} break;
					
					//! mod-speed
					case 0xCB: {
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
							trk->mGenCtrl(0x11, cdata);
					} break;
					
					//! mod-type
					case 0xCC: {
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
							trk->mGenCtrl(0x21, cdata);
					} break;
					
					//! mod-range
					case 0xCD: {
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
							trk->mGenCtrl(0x12, cdata);
					} break;
					
					//! portamento
					case 0xCE: {
						//! not bothering with this
						cdata = fgetc(rseq);
						trk->mGenCtrl(65, cdata);
					} break;
					
					//! portamento-time
					case 0xCF: {
						//! not bothering with this
						cdata = fgetc(rseq);
						trk->mGenCtrl(5, cdata);
					} break;
					
					case 0xD0: /* attack  */
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
							trk->mGenCtrl(73, cdata);
						break;
					case 0xD1: /* decay   */
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
							trk->mNRPN(0x01, 0x64, cdata);
						break;
					case 0xD2: /* sustain */
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
							trk->mGenCtrl(91, cdata);
						break;
					case 0xD3: /* release */
						//! not bothering with this
						cdata = fgetc(rseq);
						if (debugCtrls)
							trk->mGenCtrl(72, cdata);
						break;
					
					//! loop start
					case 0xD4: {
						//! just a marker command AFAIK
						//! no arguments
						trk->mGenCtrl(0x6F, 0);
					} break;
					
					//! expression
					case 0xD5: {
						//! set expression
						trk->mExp(fgetc(rseq));
					} break;
					
					//! print?
					case 0xD6: {
						//! yeah, no idea =P
						cdata = fgetc(rseq);
						if (debugCtrls)
						{
							trk->mGenCtrl(0x70, cmd & 0x7F);
							trk->mGenCtrl(0x26, cdata);
						}
					} break;
					
					//! unknown commands - 1 argument
					case 0xD8:
					case 0xD9:
					case 0xDA:
					case 0xDB: {
						//! skip arg
						cdata = fgetc(rseq);
						if (debugCtrls)
						{
							trk->mGenCtrl(0x70, cmd & 0x7F);
							trk->mGenCtrl(0x26, cdata);
						}
					} break;
					
					//! mod-delay
					case 0xE0: {
						//! not bothering with this
						u16 cdata = ReadBE(rseq, 16);
						if (debugCtrls)
							trk->mGenCtrl(0x10, cdata & 0x7F);
					} break;
					
					//! tempo
					case 0xE1: {
						//! set tempo
						trk->mTmp(ReadBE(rseq, 16));
					} break;
					
					//! sweep?
					case 0xE3: {
						//! not bothering with this
						ReadBE(rseq, 16);
						if (debugCtrls)
							trk->mGenCtrl(0x70, cmd & 0x7F);
					} break;
					
					//! loop-end
					case 0xFC: {
						//! mark AFAIK
						//! no args
						trk->mGenCtrl(0x6F, 1);
					} break;
					
					//! return
					case 0xFD: {
						//! has return adr?
						if(trk->gRPos) {
							//! seek back
							fseek(rseq, trk->gDPos = trk->gRPos, SEEK_SET);
							
							//! clear old return adr
							trk->gRPos = 0;
						}
					} break;
					
					//! track usage
					case 0xFE: {
						//! has no meaning in midi AFAIK
						//! one bit per track used
						ReadBE(rseq, 16);
						if (debugCtrls)
							trk->mGenCtrl(0x70, cmd & 0x7F);
					} break;
					
					//! end of track
					case 0xFF: {
						DebugMsg("  Trk %02u End at 0x%X.\n", i, curpos);
						//! kil trck, stop read loop
						trk->mEnd();
						loop = false;
					} break;
					
					//! O_O
					default: {
						DebugMsg("  WARNING: Unknown command %02X\n", cmd);
					} break;
				}
			}
			
			//! done \o/
			printf("  Track %02u OK\n", i);
			DebugMsg("  Trk %02u OK\n", i);
		}
	}
	
	//! write midi. yay.
	u32 trkMax = 0;
	for(int i=0;i<16;i++) if(gData.gTrack[i].gData.size()) trkMax++;
	
	//! MThd header
	//! 96-tick per quarter-note resolution
	struct {
		u32 id;
		u32 size;
		u16 format;
		u16 tracks;
		u16 time;
	} midiHeader = {
		Swap32(0x4D546864),
		Swap32(6),
		Swap16(1),
		Swap16(trkMax),
		Swap16(96),
	}; fwrite(&midiHeader, 14, 1, midi);
	
	//! process each track
	for(int i=0;i<16;i++) {
		//! have data?
		u32 len = gData.gTrack[i].gData.size();
		if(!len) continue;
		
		//! yop, create MTrk struct
		struct {
			u32 id;
			u32 len;
		} trkHead = {
			Swap32(0x4D54726B),
			Swap32(len),
		}; fwrite(&trkHead, 8, 1, midi);
		
		//! dump all data
		for(u32 j=0;j<len;j++) fputc(gData.gTrack[i].gData[j], midi);
	} fclose(midi);
}

/**************************************/

void rseqProc(const char *filename, FILE *rseq) {
	u32 tPos;
	RSEQHead_t &rcnk = gData.gRSEQHead;
	DATAHead_t &dcnk = gData.gDATAHead;
	LABLHead_t &lcnk = gData.gLABLHead;
	
	//! reset data
	gData.Reset();
	DebugMsg("  State reset successfully\n");
	
	//! write out debug message - position in code
	DebugMsg("  Attempting to read RSEQ chunk...\n");
	
	/* read RSEQ chunk */ {
		//! save position + read header
		tPos        = ftell(rseq);
		rcnk.id     = ReadLE(rseq, 32);
		rcnk.magic  = ReadBE(rseq, 32);
		rcnk.size   = ReadBE(rseq, 32);
		rcnk.cSize  = ReadBE(rseq, 16);
		rcnk.cBlock = ReadBE(rseq, 16);
		
		//! validate
		if(memcmp(&rcnk.id, "RSEQ", 4) || rcnk.magic != 0xFEFF0100) {
			//! failed
			printf("Invalid RSEQ file (bad RSEQ chunk)\n");
			DebugMsg(
				"  Bad RSEQ chunk\n"
				"    Chunk ID          = 0x%08X\n"
				"    Chunk Magic       = 0x%08X\n"
				"    Chunk size        = %u\n bytes"
				"    Chunk header size = %u bytes\n"
				"    Chunk block count = %u blocks\n",
				rcnk.id,
				rcnk.magic,
				rcnk.size,
				rcnk.cSize,
				rcnk.cBlock
			);
			return;
		}
		
		//! skip header
		fseek(rseq, tPos + rcnk.cSize, SEEK_SET);
	}
	
	//! print off debug message
	DebugMsg(
		"  RSEQ chunk OK\n"
		"    Chunk ID          = 0x%08X\n"
		"    Chunk Magic       = 0x%08X\n"
		"    Chunk size        = %u bytes\n"
		"    Chunk header size = %u bytes\n"
		"    Chunk block count = %u blocks\n",
		rcnk.id,
		rcnk.magic,
		rcnk.size,
		rcnk.cSize,
		rcnk.cBlock
	);
	
	//! process other chunks
	u32 ckLen = 0; //! just to shut GCC up
	for(u32 i=0;i<rcnk.cBlock;i++) {
		//! save position
		tPos = ftell(rseq);
		
		//! read ID
		u32 id = ReadLE(rseq, 32);
		
		//! data chunk?
		if(!memcmp(&id, "DATA", 4)) {
			//! flag up data chunk
			gData.gStat |= CHNK_HAVE_DATA;
			
			//! DATA chunk
			dcnk.id     = id;
			dcnk.size   = ckLen = ReadBE(rseq, 32);
			dcnk.offset = ReadBE(rseq, 32);
			dcnk.fOff   = tPos + dcnk.offset;
			
			//! debug stuff
			DebugMsg(
				"  Have DATA chunk\n"
				"    Chunk ID     = 0x%08X\n"
				"    Chunk size   = %u bytes\n"
				"    Chunk offset = %u bytes (relative)\n"
				"    Seq. offset  = %u bytes (absolute)\n",
				dcnk.id,
				dcnk.size,
				dcnk.offset,
				dcnk.fOff
			);
		}
		
		//! label chunk?
		else if(!memcmp(&id, "LABL", 4)) {
			//! flag up label chunk
			gData.gStat |= CHNK_HAVE_LABL;
			
			//! LABL chunk
			lcnk.id     = id;
			lcnk.size   = ckLen = ReadBE(rseq, 32);
			lcnk.labels = ReadBE(rseq, 32);
			lcnk.lOff   = tPos + 8;
			
			//! debug stuff
			DebugMsg("  Have LABL chunk\n");
			
			std::vector<u32> lOffsets;
			for (u32 i = 0; i < lcnk.labels; i ++)
			{
				u32 lpos = ReadBE(rseq, 32) + lcnk.lOff;
				lOffsets.push_back(lpos);
			}
			for (u32 i = 0; i < lcnk.labels; i ++)
			{
				fseek(rseq, lOffsets[i], SEEK_SET);
				u32 seqpos = ReadBE(rseq, 32);
				u32 lbllen = ReadBE(rseq, 32);
				char* lbldata = new char[lbllen];
				fread(lbldata, 1, lbllen, rseq);
				
				std::string lblstr = std::string(lbldata, lbllen);
				gData.gLabels[seqpos] = lblstr;
				delete[] lbldata;
			}
			DebugMsg("  Read %u labels\n", lcnk.labels);
		}
		
		//! skip chunk
		fseek(rseq, tPos + ckLen, SEEK_SET);
	}
	
	//! can be decoded?
	if(!BOOL_EQUAL(gData.gStat, CHNK_NEEDED)) {
		//! fail - not enough data to decode
		printf("Not enough data to decode with\n");
		DebugMsg("  Insufficient data (exit code 0x%02X, needed 0x%02X)\n", gData.gStat, CHNK_NEEDED);
		return;
	}
	
	//! create target MIDI filename
	char *newFN = new char[strlen(filename)+1]; strcpy(newFN, filename);
	if(char *p = strrchr(newFN, '.')) strcpy(p, ".mid");
	else strcat(newFN, ".mid");
	DebugMsg("  Writing to %s\n", newFN);
	
	//! create target MIDI file
	FILE *midi = fopen(newFN, "wb"); delete newFN;
	if(!midi) {
		//! failed to open target
		printf("  Cannot open output Midi file\n");
		DebugMsg("  Can't open target\n");
		delete newFN;
		return;
	}
	
	//! start processing
	rseqDo(midi, rseq);
}

/**************************************/

int main(int argc, char *argv[]) {
	int firstarg;
	
	//! need at least two args
	if(argc < 2) {
		//! print msg
		printf(
			"rseq2midi\n"
			"Usage: rseq2midi [-i] file1.rseq [file2.rseq [file3.rseq [...]]]\n"
			"-i - ignore jump commands\n"
		);
		
		//! failed
		return 1;
	}
	
	firstarg = 1;
	ignoreJumps = false;
	debugCtrls = false;
	
	for(;firstarg<argc;firstarg++)
	{
		if (! strcmp(argv[firstarg], "-i"))
			ignoreJumps = true;
		else if (! strcmp(argv[firstarg], "-d"))
			debugCtrls = true;
		else
			break;
	}
	
	//! read every arg
	for(int i=firstarg;i<argc;i++) {
		//! print to console + debug
		printf("%s:\n", argv[i]);
		DebugMsg("%s:\n", argv[i]);
		
		//! open file
		FILE *rseq = fopen(argv[i], "rb");
		if(!rseq) {
			//! can't open - skip
			printf("  Couldn't open file\n");
			DebugMsg("  Failed\n");
			continue;
		}
		
		//! process file
		rseqProc(argv[i], rseq);
		
		//! close file
		fclose(rseq);
	}
	
	return 0;
}

/**************************************/
/* EOF                                */
/**************************************/
