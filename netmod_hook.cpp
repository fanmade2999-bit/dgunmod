// netmod_hook.cpp -- libnetmod.so <-> libapp_lib.so (ARM64 frame hook)
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef __ANDROID__
#include <android/log.h>
#define NM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "netmod", __VA_ARGS__)
#else
#define NM_LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif
struct _PWORK;
namespace off {
constexpr uintptr_t kLibPrgPLY=0xbda9c,kPrgTable=0x12bbf8;
constexpr int kKindPLY=2,kProxyKind=24;
constexpr uintptr_t kInUse=0,kFlags=0x02,kBody=0x08,kParam=0x10,kKind=0x1a;
constexpr uintptr_t kBodyX=0x0C,kBodyY=0x10,kBodyZ=0x14,kParamAng=0x04;
constexpr uint16_t kTeamMask=0x00C0;
}
using prg_fn_t=void(*)(_PWORK*);
using pw_aloc_t=_PWORK*(*)(_PWORK*,int,int);
using pw_set_pos_t=_PWORK*(*)(_PWORK*,float,float,float,int);
using pw_set_dir_t=void(*)(_PWORK*,float);
using pw_free_t=void(*)(_PWORK*);
#include "netmod_api.h"
constexpr int kProxyTypeOverride=-1,kProxyAllocFlags=0,kSetPosMode=0;
constexpr float kSnapDistance=50.0f,kSmoothTauSec=0.06f;
namespace {
uintptr_t g_base=0; prg_fn_t* g_table_slot=nullptr; prg_fn_t g_orig_prg=nullptr;
_PWORK** g_pwk_sym=nullptr; pw_aloc_t g_pw_aloc=nullptr; pw_set_pos_t g_pw_set_pos=nullptr;
pw_set_dir_t g_pw_set_dir=nullptr; pw_free_t g_pw_free=nullptr;
std::chrono::steady_clock::time_point g_last_proxy_log;
uintptr_t g_relro_lo=0,g_relro_hi=0; _PWORK* g_proxy=nullptr; int16_t g_proxy_type=0;
bool g_have_remote=false; RemoteTransform g_target{}; float g_cur_x=0,g_cur_y=0,g_cur_z=0,g_cur_ang=0;
std::chrono::steady_clock::time_point g_last_tick; std::atomic<bool> g_installed{false};
inline uint8_t* B(_PWORK* p){return reinterpret_cast<uint8_t*>(p);}
template<class T> inline T& F(_PWORK* p,uintptr_t o){return *reinterpret_cast<T*>(B(p)+o);}
inline uint8_t* body(_PWORK* p){return F<uint8_t*>(p,off::kBody);}
inline uint8_t* param(_PWORK* p){return F<uint8_t*>(p,off::kParam);}
inline bool slot_live(_PWORK* p){return p&&F<int16_t>(p,off::kInUse)>=1;}
void log_anim_state(const char* tag,_PWORK* w){
 if(!w){NM_LOG("%s anim: PWORK=null",tag);return;}
 uint8_t* d=param(w);if(!d){NM_LOG("%s anim: pdisp=null",tag);return;}
 void* model=*reinterpret_cast<void**>(d+0x28);
 int model_count=model?static_cast<int>(*reinterpret_cast<int16_t*>(static_cast<uint8_t*>(model)+0x06)):-1;
 void* panm=*reinterpret_cast<void**>(d+0x30);
 if(!panm){NM_LOG("%s anim: model=%p model_count=%d panm0=null",tag,model,model_count);return;}
 auto* a=static_cast<uint8_t*>(panm);
 NM_LOG("%s anim: model=%p model_count=%d panm0=%p flags=0x%04x requested=%d current=%d key=%d phase=%.3f clip=%p",tag,model,model_count,panm,static_cast<unsigned>(*reinterpret_cast<uint16_t*>(a+0x02)),static_cast<int>(*reinterpret_cast<int16_t*>(a+0x04)),static_cast<int>(*reinterpret_cast<int16_t*>(a+0x06)),static_cast<int>(*reinterpret_cast<int16_t*>(a+0x08)),*reinterpret_cast<float*>(a+0x14),*reinterpret_cast<void**>(a+0x28));
}
void mirror_requested_animation(_PWORK* host,_PWORK* proxy){
 if(!host||!proxy)return;
 uint8_t* hd=param(host);uint8_t* pd=param(proxy);
 if(!hd||!pd)return;
 void* hm=*reinterpret_cast<void**>(hd+0x28);
 void* pm=*reinterpret_cast<void**>(pd+0x28);
 if(!hm||hm!=pm)return;
 const int count=static_cast<int>(*reinterpret_cast<int16_t*>(static_cast<uint8_t*>(hm)+0x06));
 if(count<=0||count>256)return;
 auto* ha=static_cast<uint8_t*>(*reinterpret_cast<void**>(hd+0x30));
 auto* pa=static_cast<uint8_t*>(*reinterpret_cast<void**>(pd+0x30));
 if(!ha||!pa)return;
 const int requested=static_cast<int>(*reinterpret_cast<int16_t*>(ha+0x04));
 const int previous=static_cast<int>(*reinterpret_cast<int16_t*>(pa+0x04));
 if(requested<0||requested>=count||requested==previous)return;
 *reinterpret_cast<int16_t*>(pa+0x04)=static_cast<int16_t>(requested);
 NM_LOG("anim mirror: host requested=%d proxy requested %d -> %d model_count=%d",requested,previous,requested,count);
}
inline _PWORK* local_player(){return g_pwk_sym?*g_pwk_sym:nullptr;}
float wrap_pi(float a){const float pi=3.14159265358979f;while(a>pi)a-=2*pi;while(a<-pi)a+=2*pi;return a;}
struct LibInfo{bool found=false;uintptr_t bias=0,lo=0,hi=0,relro_lo=0,relro_hi=0;};
int phdr_cb(dl_phdr_info* i,size_t,void* d){
 auto* o=static_cast<LibInfo*>(d); if(!i->dlpi_name||!std::strstr(i->dlpi_name,"libapp_lib.so"))return 0;
 const long ps=sysconf(_SC_PAGESIZE); const uintptr_t pm=ps>0?static_cast<uintptr_t>(ps)-1:0xfff;
 uintptr_t lo=UINTPTR_MAX,hi=0;o->bias=i->dlpi_addr;
 for(int n=0;n<i->dlpi_phnum;++n){const ElfW(Phdr)& ph=i->dlpi_phdr[n];const uintptr_t a=i->dlpi_addr+ph.p_vaddr;
  if(ph.p_type==PT_LOAD){lo=std::min<uintptr_t>(lo,a);hi=std::max<uintptr_t>(hi,a+ph.p_memsz);}
  else if(ph.p_type==PT_GNU_RELRO){o->relro_lo=(a+pm)&~pm;o->relro_hi=(a+ph.p_memsz)&~pm;}
 } o->lo=lo;o->hi=hi;o->found=lo<hi;return 1;
}
bool write_slot(prg_fn_t v){
 if(!g_table_slot){NM_LOG("write_slot: no table slot");return false;} const long ps=sysconf(_SC_PAGESIZE);
 if(ps<=0){NM_LOG("write_slot: bad page size %ld",ps);return false;} const uintptr_t page=reinterpret_cast<uintptr_t>(g_table_slot)&~static_cast<uintptr_t>(ps-1);
 if(mprotect(reinterpret_cast<void*>(page),static_cast<size_t>(ps),PROT_READ|PROT_WRITE)!=0){NM_LOG("write_slot: mprotect(RW) failed errno=%d (%s)",errno,std::strerror(errno));return false;}
 __atomic_store_n(reinterpret_cast<void**>(g_table_slot),reinterpret_cast<void*>(v),__ATOMIC_RELEASE);
 if(page>=g_relro_lo&&page+static_cast<uintptr_t>(ps)<=g_relro_hi)mprotect(reinterpret_cast<void*>(page),static_cast<size_t>(ps),PROT_READ);
 return true;
}
_PWORK* ensure_proxy(_PWORK* host){
 if(!host){NM_LOG("ensure_proxy: host is null");return nullptr;}
 if(g_proxy&&F<int16_t>(g_proxy,off::kInUse)==g_proxy_type&&F<int8_t>(g_proxy,off::kKind)==off::kProxyKind)return g_proxy;
 g_proxy=nullptr;if(!g_pw_aloc){NM_LOG("ensure_proxy: pw_aloc unavailable");return nullptr;}
 int type=(kProxyTypeOverride>=1)?kProxyTypeOverride:static_cast<int>(F<int16_t>(host,off::kInUse));
 if(type<1||type>0x41){NM_LOG("ensure_proxy: invalid allocation type %d",type);return nullptr;}
 _PWORK* p=g_pw_aloc(nullptr,type,kProxyAllocFlags);if(!p){NM_LOG("pw_aloc(type %d) returned null (pool full / type not loaded)",type);return nullptr;}
 F<int8_t>(p,off::kKind)=static_cast<int8_t>(off::kProxyKind);g_proxy=p;g_proxy_type=static_cast<int16_t>(type);g_have_remote=false;
 NM_LOG("proxy allocated: pw=%p type=%d kind=%d",static_cast<void*>(g_proxy),type,off::kProxyKind);
 NM_LOG("PWORK compare: host=%p inUse=%d flags=0x%04x body=%p param=%p kind=%d | proxy=%p inUse=%d flags=0x%04x body=%p param=%p kind=%d",
  static_cast<void*>(host),static_cast<int>(F<int16_t>(host,off::kInUse)),static_cast<unsigned>(F<uint16_t>(host,off::kFlags)),
  static_cast<void*>(body(host)),static_cast<void*>(param(host)),static_cast<int>(F<int8_t>(host,off::kKind)),
  static_cast<void*>(g_proxy),static_cast<int>(F<int16_t>(g_proxy,off::kInUse)),static_cast<unsigned>(F<uint16_t>(g_proxy,off::kFlags)),
  static_cast<void*>(body(g_proxy)),static_cast<void*>(param(g_proxy)),static_cast<int>(F<int8_t>(g_proxy,off::kKind)));
 if(body(host))NM_LOG("host body xyz=(%.1f,%.1f,%.1f)",*reinterpret_cast<float*>(body(host)+off::kBodyX),*reinterpret_cast<float*>(body(host)+off::kBodyY),*reinterpret_cast<float*>(body(host)+off::kBodyZ));
 if(body(g_proxy))NM_LOG("proxy body xyz=(%.1f,%.1f,%.1f)",*reinterpret_cast<float*>(body(g_proxy)+off::kBodyX),*reinterpret_cast<float*>(body(g_proxy)+off::kBodyY),*reinterpret_cast<float*>(body(g_proxy)+off::kBodyZ));
 auto log_disp=[](const char* tag,_PWORK* w){uint8_t* d=param(w);if(!d){NM_LOG("%s pdisp=null",tag);return;}NM_LOG("%s pdisp=%p live=%d flags=0x%04x screen=(%.1f,%.1f,%.1f) ext=(%.1f,%.1f,%.1f) z9d=%u",tag,static_cast<void*>(d),static_cast<int>(*reinterpret_cast<int16_t*>(d)),static_cast<unsigned>(*reinterpret_cast<uint16_t*>(d+2)),*reinterpret_cast<float*>(d+0x18),*reinterpret_cast<float*>(d+0x1c),*reinterpret_cast<float*>(d+0x20),*reinterpret_cast<float*>(d+0x70),*reinterpret_cast<float*>(d+0x74),*reinterpret_cast<float*>(d+0x78),static_cast<unsigned>(d[0x9d]));};
 log_disp("host",host);log_disp("proxy",g_proxy);
 return g_proxy;
}
constexpr bool kPvP=false;
void apply_team_flags(_PWORK* p,_PWORK* h){uint16_t& pf=F<uint16_t>(p,off::kFlags);if(kPvP)pf=static_cast<uint16_t>((pf&~0x00E0)|0x0080);else{const uint16_t hf=F<uint16_t>(h,off::kFlags);pf=static_cast<uint16_t>((pf&~off::kTeamMask)|(hf&off::kTeamMask));}}
void sync_remote(_PWORK* host,float dt){
 RemoteTransform rt;if(netmod_get_remote_transform(&rt)){g_target=rt;if(!g_have_remote){g_have_remote=true;}}
 if(!g_have_remote)return;_PWORK* p=ensure_proxy(host);if(!p)return;if(!g_pw_set_pos)return;
 uint8_t* host_body=body(host);if(!host_body)return;
 // Diagnostic mode: sender coordinates are offsets from the local player, not absolute world coordinates.
 const float want_x=*reinterpret_cast<float*>(host_body+off::kBodyX)+g_target.x;
 const float want_y=*reinterpret_cast<float*>(host_body+off::kBodyY)+g_target.y;
 const float want_z=*reinterpret_cast<float*>(host_body+off::kBodyZ)+g_target.z;
 if(!g_have_remote)return;
 if(g_cur_x==0.0f&&g_cur_y==0.0f&&g_cur_z==0.0f){g_cur_x=want_x;g_cur_y=want_y;g_cur_z=want_z;}
 const float dx=want_x-g_cur_x,dy=want_y-g_cur_y,dz=want_z-g_cur_z;if(std::sqrt(dx*dx+dy*dy+dz*dz)>kSnapDistance){g_cur_x=want_x;g_cur_y=want_y;g_cur_z=want_z;}
 else{const float a=1.0f-std::exp(-dt/kSmoothTauSec);g_cur_x+=dx*a;g_cur_y+=dy*a;g_cur_z+=dz*a;}
 // Combined local diagnostic: keep the proxy near the player and mirror the player's live facing angle.
 // The test sender currently supplies a fixed angle, so its packet angle cannot validate turning.
 uint8_t* host_param=param(host);
 const float host_ang=host_param?*reinterpret_cast<float*>(host_param+off::kParamAng):g_target.angle;
 g_cur_ang=wrap_pi(host_ang);
 g_pw_set_pos(p,g_cur_x,g_cur_y,g_cur_z,kSetPosMode);
 if(g_pw_set_dir)g_pw_set_dir(p,g_cur_ang);
 else if(uint8_t* prm=param(p))*reinterpret_cast<float*>(prm+off::kParamAng)=g_cur_ang;
 mirror_requested_animation(host,p);
 const auto now=std::chrono::steady_clock::now();
 if(g_last_proxy_log.time_since_epoch().count()==0||std::chrono::duration<float>(now-g_last_proxy_log).count()>=1.0f){
  g_last_proxy_log=now;
  const float proxy_ang=param(p)?*reinterpret_cast<float*>(param(p)+off::kParamAng):0.0f;
  NM_LOG("proxy update: pw=%p host=(%.1f,%.1f,%.1f) offset=(%.1f,%.1f,%.1f) target=(%.1f,%.1f,%.1f) host_angle=%.3f packet_angle=%.3f applied_angle=%.3f proxy_angle=%.3f seq=%u kind=%d",
   static_cast<void*>(p),*reinterpret_cast<float*>(host_body+off::kBodyX),*reinterpret_cast<float*>(host_body+off::kBodyY),*reinterpret_cast<float*>(host_body+off::kBodyZ),
   g_target.x,g_target.y,g_target.z,want_x,want_y,want_z,host_ang,g_target.angle,g_cur_ang,proxy_ang,g_target.seq,off::kProxyKind);
  log_anim_state("host",host);log_anim_state("proxy",p);
 }
 apply_team_flags(p,host);
}
void hk_prg_PLY(_PWORK* pw){
 static bool logged_first_call=false;if(!logged_first_call){NM_LOG("hk_prg_PLY EXECUTED: first call received (pw=%p)",pw);logged_first_call=true;}
 prg_fn_t orig=g_orig_prg;_PWORK* lp=local_player();static bool logged_dispatch=false;if(!logged_dispatch){NM_LOG("hk_prg_PLY dispatch: pw=%p local_player=%p orig=%p match=%s",pw,lp,reinterpret_cast<void*>(orig),(pw==lp)?"YES":"NO");logged_dispatch=true;}
 if(!orig)return;if(pw!=lp){orig(pw);return;}orig(pw);
 const auto now=std::chrono::steady_clock::now();float dt=std::chrono::duration<float>(now-g_last_tick).count();g_last_tick=now;if(dt<=0.0f||dt>0.25f)dt=1.0f/60.0f;
 uint8_t* bd=body(pw);uint8_t* pr=param(pw);if(bd&&pr)netmod_update_local_transform(*reinterpret_cast<float*>(bd+off::kBodyX),*reinterpret_cast<float*>(bd+off::kBodyY),*reinterpret_cast<float*>(bd+off::kBodyZ),*reinterpret_cast<float*>(pr+off::kParamAng));
 sync_remote(pw,dt);
}
}
extern "C" bool netmod_hook_install(){
 if(g_installed.load())return true;NM_LOG("install: start");void* h=dlopen("libapp_lib.so",RTLD_NOW|RTLD_NOLOAD);NM_LOG("install: dlopen(libapp_lib.so, NOLOAD) -> %p",h);if(!h)return false;
 void* prg=dlsym(h,"_Z7prg_PLYP6_PWORK");void* pwk=dlsym(h,"pwk");g_pw_aloc=reinterpret_cast<pw_aloc_t>(dlsym(h,"_Z7pw_alocP6_PWORKii"));g_pw_set_pos=reinterpret_cast<pw_set_pos_t>(dlsym(h,"_Z10pw_set_posP6_PWORKfffi"));g_pw_set_dir=reinterpret_cast<pw_set_dir_t>(dlsym(h,"_Z10pw_set_dirP6_PWORKf"));g_pw_free=reinterpret_cast<pw_free_t>(dlsym(h,"_Z7pw_freeP6_PWORK"));
 NM_LOG("install: prg_PLY=%p pwk=%p pw_aloc=%p pw_set_pos=%p pw_set_dir=%p pw_free=%p",prg,pwk,reinterpret_cast<void*>(g_pw_aloc),reinterpret_cast<void*>(g_pw_set_pos),reinterpret_cast<void*>(g_pw_set_dir),reinterpret_cast<void*>(g_pw_free));if(!prg||!pwk||!g_pw_set_pos)return false;g_pwk_sym=static_cast<_PWORK**>(pwk);
 LibInfo li;dl_iterate_phdr(phdr_cb,&li);if(!li.found)return false;const uintptr_t bias_from_sym=reinterpret_cast<uintptr_t>(prg)-off::kLibPrgPLY;NM_LOG("install: bias(phdr)=%p bias(sym)=%p range=[%p,%p) relro=[%p,%p)",reinterpret_cast<void*>(li.bias),reinterpret_cast<void*>(bias_from_sym),reinterpret_cast<void*>(li.lo),reinterpret_cast<void*>(li.hi),reinterpret_cast<void*>(li.relro_lo),reinterpret_cast<void*>(li.relro_hi));if(bias_from_sym!=li.bias)return false;
 g_base=li.bias;g_relro_lo=li.relro_lo;g_relro_hi=li.relro_hi;const uintptr_t slot_addr=g_base+off::kPrgTable+off::kKindPLY*sizeof(void*);if((slot_addr&(sizeof(void*)-1))!=0||slot_addr<li.lo||slot_addr+sizeof(void*)>li.hi)return false;g_table_slot=reinterpret_cast<prg_fn_t*>(slot_addr);const uintptr_t cur=*reinterpret_cast<volatile uintptr_t*>(slot_addr);if(cur!=reinterpret_cast<uintptr_t>(prg)){NM_LOG("install: table slot != prg_PLY -- wrong library build, not patching");g_table_slot=nullptr;return false;}
 g_orig_prg=reinterpret_cast<prg_fn_t>(cur);g_last_tick=std::chrono::steady_clock::now();if(!write_slot(&hk_prg_PLY)){g_orig_prg=nullptr;g_table_slot=nullptr;return false;}g_installed.store(true);NM_LOG("hooked table[2] @ %p (base %p)",static_cast<void*>(g_table_slot),reinterpret_cast<void*>(g_base));return true;
}
extern "C" void netmod_hook_uninstall(){if(!g_installed.exchange(false))return;if(!write_slot(g_orig_prg))NM_LOG("uninstall: could not restore table slot");if(slot_live(g_proxy)&&g_pw_free)g_pw_free(g_proxy);g_proxy=nullptr;}
