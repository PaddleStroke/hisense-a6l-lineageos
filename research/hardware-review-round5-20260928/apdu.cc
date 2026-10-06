// Exact FilesystemChannel READ BINARY method, Android CHECK modeled as fatal.
// Runs in a separate process; only dummy memory-backed SIM contents exist.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
struct Log {template<class T>Log& operator<<(const T&){return *this;}};
#define LOG(x) Log{}
struct Check {bool ok;~Check(){if(!ok)std::abort();}template<class T>Check& operator<<(const T&){return *this;}};
#define CHECK(x) Check{bool(x)}
namespace aidl {struct IccIoResult {int sw=0x9000;std::string data;};}
static aidl::IccIoResult toIccIoResult(const std::string& s){return {0x9000,s};}
static aidl::IccIoResult toIccIoResult(int sw){return {sw,{}};}
constexpr int IO_RESULT_FILE_NOT_FOUND=0x6a82;
struct Filesystem {struct Path{std::string toString()const{return "EF_PL";}};
 std::optional<std::string> read(Path)const{return "en";}};
struct FilesystemApp {struct FilesystemChannel {
 std::shared_ptr<Filesystem> mFilesystem=std::make_shared<Filesystem>();
 Filesystem::Path mSelectedFile;
 aidl::IccIoResult commandReadBinary(int32_t,int32_t)const;
};};
#include "apdu_method.inc"
int main(int argc,char** argv){
 int offset=argc>1?atoi(argv[1]):0;
 printf("READ_BINARY offset=%d dummy_file_length=2\n",offset);fflush(stdout);
 FilesystemApp::FilesystemChannel c;
 auto r=c.commandReadBinary(0,offset);
 printf("returned_sw=%x dummy_data=%s\n",r.sw,r.data.c_str());
}
