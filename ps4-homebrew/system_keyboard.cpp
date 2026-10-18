// SPDX-License-Identifier: GPL-3.0-or-later
// PS4 system text-entry dialog for game requested virtual keyboards.
#include "platform/platform.h"
#include "util/log.h"
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
namespace {
using CommonInitFn=int(*)(); using ImeInitFn=int(*)(const void*,void*);
using ImeStatusFn=int(*)(); using ImeResultFn=int(*)(void*); using ImeTermFn=int(*)();
CommonInitFn g_commonInit=nullptr; ImeInitFn g_imeInit=nullptr;
ImeStatusFn g_imeStatus=nullptr; ImeResultFn g_imeResult=nullptr; ImeTermFn g_imeTerm=nullptr;
}
namespace xc::platform { namespace {
struct ImeDialogParam { int32_t userId,type; uint64_t supportedLanguages; int32_t enterLabel,inputMethod; void* filter; uint32_t option,maxTextLength; char16_t* inputTextBuffer; float posx,posy; int32_t horizontalAlignment,verticalAlignment; const char16_t* placeholder; const char16_t* title; int8_t reserved[16]; };
static_assert(sizeof(ImeDialogParam)==96,"PS4 IME dialog ABI mismatch");
struct DialogResult { int32_t endStatus; int8_t reserved[12]; };
int g_available=-1; bool g_open=false; std::u16string g_title; std::vector<char16_t> g_buffer;
std::string systemLib(const char* name){const char* word=sceKernelGetFsSandboxRandomWord();return std::string("/")+(word?word:"")+"/common/lib/"+name;}
int load(const std::string& path){int result=0;auto h=sceKernelLoadStartModule(path.c_str(),0,nullptr,0,nullptr,&result);XC_LOGI("keyboard: module load result=0x%08x",static_cast<unsigned>(h));return static_cast<int>(h);}
std::u16string toUtf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) c = (c << 6) | (static_cast<uint8_t>(s[i]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

std::string toUtf8(const char16_t* s) {
    std::string out;
    for (; *s; ++s) {
        uint32_t c = *s;
        if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
            ++s;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool resolve(){if(g_available>=0)return g_available!=0;g_available=0;int common=load(systemLib("libSceCommonDialog.sprx"));if(common<0)common=load("libSceCommonDialog.sprx");
if(common<0){XC_LOGW("keyboard: common dialog module unavailable");return false;}
void* sym=nullptr;int drc=sceKernelDlsym(common,"sceCommonDialogInitialize",&sym);g_commonInit=reinterpret_cast<CommonInitFn>(sym);
if(drc<0||!g_commonInit){XC_LOGW("keyboard: common dialog symbol unavailable rc=0x%08x",unsigned(drc));return false;}
int rc=g_commonInit();XC_LOGI("keyboard: common dialog init rc=0x%08x",unsigned(rc));
int ime=load(systemLib("libSceImeDialog.sprx"));if(ime<0)ime=load("libSceImeDialog.sprx");if(ime<0){XC_LOGW("keyboard: IME module unavailable");return false;}
auto resolveSym=[&](const char* name,void** out){int r=sceKernelDlsym(ime,name,out);if(r<0)XC_LOGW("keyboard: symbol lookup failed rc=0x%08x",unsigned(r));return r==0&&*out;};
void *a=nullptr,*b=nullptr,*c=nullptr,*d=nullptr;bool ok=resolveSym("sceImeDialogInit",&a)&&resolveSym("sceImeDialogGetStatus",&b)&&resolveSym("sceImeDialogGetResult",&c)&&resolveSym("sceImeDialogTerm",&d);
g_imeInit=reinterpret_cast<ImeInitFn>(a);g_imeStatus=reinterpret_cast<ImeStatusFn>(b);g_imeResult=reinterpret_cast<ImeResultFn>(c);g_imeTerm=reinterpret_cast<ImeTermFn>(d);g_available=ok?1:0;return ok;}
}
bool systemKeyboardAvailable(){return resolve();}
bool openSystemKeyboard(const std::string& title,const std::string& text,size_t maxLength,KeyboardKind kind){if(g_open||!resolve())return false;if(maxLength==0||maxLength>2048)maxLength=2048;g_title=toUtf16(title);auto initial=toUtf16(text);if(initial.size()>maxLength)initial.resize(maxLength);g_buffer.assign(maxLength+1,u'\0');std::copy(initial.begin(),initial.end(),g_buffer.begin());ImeDialogParam p{};int32_t user=-1;int urc=sceUserServiceGetInitialUser(&user);if(urc<0){XC_LOGW("keyboard: user lookup rc=0x%08x",unsigned(urc));return false;}p.userId=user;p.maxTextLength=uint32_t(maxLength);p.inputTextBuffer=g_buffer.data();p.posx=960;p.posy=540;p.horizontalAlignment=1;p.verticalAlignment=1;p.title=g_title.c_str();switch(kind){case KeyboardKind::Text:break;case KeyboardKind::Password:p.option=4;break;case KeyboardKind::Number:p.type=4;break;case KeyboardKind::Email:p.type=3;break;case KeyboardKind::Url:p.type=2;break;}int rc=g_imeInit(&p,nullptr);XC_LOGI("keyboard: sceImeDialogInit rc=0x%08x",unsigned(rc));g_open=rc==0;return g_open;}
KeyboardStatus pollSystemKeyboard(std::string& text){if(!g_open)return KeyboardStatus::Closed;int status=g_imeStatus();if(status==1)return KeyboardStatus::Open;DialogResult r{};int rc=g_imeResult(&r);int termRc=g_imeTerm();XC_LOGI("keyboard: dialog termination rc=0x%08x",unsigned(termRc));if(termRc!=0)return KeyboardStatus::Open;g_open=false;XC_LOGI("keyboard: closed status=%d result=0x%08x end=%d",status,unsigned(rc),r.endStatus);if(status!=2||rc!=0||r.endStatus!=0)return KeyboardStatus::Cancelled;text=toUtf8(g_buffer.data());return KeyboardStatus::Accepted;}
} // namespace xc::platform
