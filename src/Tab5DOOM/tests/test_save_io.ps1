param(
    [string]$Compiler = 'gcc',
    [string]$Output = "$env:TEMP/tab5_save_io_test.exe"
)
$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw -LiteralPath "$PSScriptRoot/../components/prboom/m_misc.c"
$write = [regex]::Match($source, '(?s)boolean M_WriteFile\(.*?(?=\r?\n/\*)').Value
$read = [regex]::Match($source, '(?s)int M_ReadFile\(.*?(?=\r?\n//)').Value
if (!$write -or !$read) { throw 'Cannot locate actual save I/O functions' }
$prefix = @'
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
typedef int boolean;
typedef unsigned char byte;
#define PU_STATIC 1
#define LO_INFO 1
#define lprintf(...) ((void)0)
static int allocations, short_write, short_read, close_error, promotion_error;
static void *Z_Malloc(size_t n, int tag, void *user) {
    (void)tag; (void)user; void *p=malloc(n); assert(p); allocations++; return p;
}
static void Z_Free(void *p) { if(p) { allocations--; free(p); } }
static void I_BeginRead(void) {}
static void I_EndRead(void) {}
static size_t test_fwrite(const void *p,size_t s,size_t n,FILE *f) {
    return fwrite(p,s,short_write && n ? n-1:n,f);
}
static size_t test_fread(void *p,size_t s,size_t n,FILE *f) {
    return fread(p,s,short_read && n ? n-1:n,f);
}
static int test_fclose(FILE *f) { int r=fclose(f); return close_error ? EOF:r; }
static int test_rename(const char *a,const char *b) {
    if(promotion_error && strstr(a,".tmp")) {errno=EIO;return -1;}
    return rename(a,b);
}
#define fwrite test_fwrite
#define fread test_fread
#define fclose test_fclose
#define rename test_rename
'@
$tests = @'
static void verify(const char *path,const byte *data,int size) {
    byte *buf=NULL; assert(M_ReadFile(path,&buf)==size);
    assert(buf && memcmp(buf,data,size)==0); Z_Free(buf); assert(allocations==0);
}
int main(void) {
    const char *path="tab5-save-io-test.dsg";
    byte original[32768], replacement[65537], *buf=(void *)1;
    memset(original,0x35,sizeof(original)); memset(replacement,0xa7,sizeof(replacement));
    remove(path); remove("tab5-save-io-test.dsg.tmp"); remove("tab5-save-io-test.dsg.bak");
    assert(M_ReadFile(path,&buf)==-1 && buf==NULL);
    assert(!M_WriteFile(path,original,0));
    assert(!M_WriteFile(path,NULL,10));
    assert(!M_WriteFile("tab5-save-missing-directory/slot.dsg",original,sizeof(original)));
    assert(M_WriteFile(path,original,sizeof(original))); verify(path,original,sizeof(original));
    short_write=1; assert(!M_WriteFile(path,replacement,sizeof(replacement))); short_write=0;
    verify(path,original,sizeof(original));
    close_error=1; assert(!M_WriteFile(path,replacement,sizeof(replacement))); close_error=0;
    verify(path,original,sizeof(original));
    promotion_error=1; assert(!M_WriteFile(path,replacement,sizeof(replacement))); promotion_error=0;
    verify(path,original,sizeof(original));
    assert(M_WriteFile(path,replacement,sizeof(replacement))); verify(path,replacement,sizeof(replacement));
    short_read=1; assert(M_ReadFile(path,&buf)==-1 && buf==NULL); short_read=0;
    assert(allocations==0);
    FILE *f=fopen("tab5-save-empty-test.dsg","wb"); assert(f); fclose(f);
    assert(M_ReadFile("tab5-save-empty-test.dsg",&buf)==-1 && buf==NULL);
    remove(path); remove("tab5-save-empty-test.dsg");
    struct stat st;
    assert(stat("tab5-save-io-test.dsg.tmp",&st)!=0);
    assert(stat("tab5-save-io-test.dsg.bak",&st)!=0);
    puts("Save I/O: create/overwrite/read/missing/empty/short-write/close-error/rename-rollback/short-read PASS");
}
'@
($prefix + "`n" + $write + "`n" + $read + "`n" + $tests) |
    & $Compiler -x c -std=c11 -Wall -Wextra -Werror -Wno-sign-compare - -o $Output
if ($LASTEXITCODE -ne 0) { throw 'Save I/O host test compilation failed' }
Push-Location (Split-Path -Parent $Output)
try {
    & $Output
    if ($LASTEXITCODE -ne 0) { throw 'Save I/O host tests failed' }
} finally { Pop-Location }
