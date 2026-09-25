#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")

static void SaveBmp(HBITMAP bmp, const wchar_t* path, HDC screen){
    BITMAP bm; GetObject(bmp,sizeof(bm),&bm);
    BITMAPINFOHEADER bh={0}; bh.biSize=sizeof(bh); bh.biWidth=bm.bmWidth;
    bh.biHeight=bm.bmHeight; bh.biPlanes=1; bh.biBitCount=24;
    HDC dc=CreateCompatibleDC(screen); SelectObject(dc,bmp);
    int stride=((bm.bmWidth*3+3)&~3); int sz=stride*bm.bmHeight;
    unsigned char* buf=new unsigned char[sz];
    GetDIBits(dc,bmp,0,bm.bmHeight,buf,(BITMAPINFO*)&bh,DIB_RGB_COLORS);
    BITMAPFILEHEADER bf={0}; bf.bfType=0x4D42; bf.bfOffBits=sizeof(bf)+sizeof(bh);
    bf.bfSize=bf.bfOffBits+sz;
    FILE* f=_wfopen(path,L"wb"); fwrite(&bf,sizeof(bf),1,f);
    fwrite(&bh,sizeof(bh),1,f); fwrite(buf,sz,1,f); fclose(f);
    delete[] buf; DeleteDC(dc);
}
static HBITMAP MakeRegBmp() {
    HDC screen = GetDC(nullptr);
    HDC cdc = CreateCompatibleDC(screen), mdc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, 16, 16);
    HBITMAP mask  = CreateBitmap(16,16,1,1,nullptr);
    HBITMAP oc=(HBITMAP)SelectObject(cdc,color);
    HBITMAP om=(HBITMAP)SelectObject(mdc,mask);
    PatBlt(cdc,0,0,16,16,WHITENESS);
    PatBlt(mdc,0,0,16,16,BLACKNESS);
    POINT T[4]={{8,2},{14,5},{8,8},{2,5}};
    POINT L[4]={{2,5},{8,8},{8,14},{2,11}};
    POINT R[4]={{8,8},{14,5},{14,11},{8,14}};
    HBRUSH cT=CreateSolidBrush(RGB(150,205,255)),cL=CreateSolidBrush(RGB(30,95,185)),
           cR=CreateSolidBrush(RGB(65,145,225)),cB=CreateSolidBrush(RGB(0,0,0));
    SelectObject(cdc,GetStockObject(NULL_PEN));
    SelectObject(cdc,cT); Polygon(cdc,T,4);
    SelectObject(cdc,cL); Polygon(cdc,L,4);
    SelectObject(cdc,cR); Polygon(cdc,R,4);
    SelectObject(mdc,GetStockObject(NULL_PEN)); SelectObject(mdc,cB);
    Polygon(mdc,T,4); Polygon(mdc,L,4); Polygon(mdc,R,4);
    SelectObject(cdc,oc); SelectObject(mdc,om);
    DeleteDC(cdc); DeleteDC(mdc);
    SaveBmp(color,L"C:\\colortest.bmp",screen);
    SaveBmp(mask,L"C:\\masktest.bmp",screen);
    DeleteObject(mask);
    DeleteObject(cT);DeleteObject(cL);DeleteObject(cR);DeleteObject(cB);
    ReleaseDC(nullptr,screen);
    return color;
}

int main(){
    HBITMAP reg = MakeRegBmp();
    HIMAGELIST il = ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,4,1);
    SHFILEINFOW sfi;
    SHGetFileInfoW(L"x",FILE_ATTRIBUTE_DIRECTORY,&sfi,sizeof(sfi),
        SHGFI_ICON|SHGFI_SMALLICON|SHGFI_USEFILEATTRIBUTES);
    ImageList_AddIcon(il,sfi.hIcon); DestroyIcon(sfi.hIcon);
    SHGetFileInfoW(L"x",FILE_ATTRIBUTE_NORMAL,&sfi,sizeof(sfi),
        SHGFI_ICON|SHGFI_SMALLICON|SHGFI_USEFILEATTRIBUTES);
    ImageList_AddIcon(il,sfi.hIcon); DestroyIcon(sfi.hIcon);
    if(reg){ ImageList_Add(il,reg,NULL); }
    int cnt = ImageList_GetImageCount(il);
    printf("imagecount=%d\n",cnt);
    // draw to bmp
    HDC screen=GetDC(nullptr);
    HDC dc=CreateCompatibleDC(screen);
    HBITMAP bmp=CreateCompatibleBitmap(screen,16*cnt,16);
    HBITMAP oldb=(HBITMAP)SelectObject(dc,bmp);
    RECT rr={0,0,16*cnt,16}; FillRect(dc,&rr,(HBRUSH)GetStockObject(WHITE_BRUSH));
    for(int i=0;i<cnt;++i) ImageList_Draw(il,i,dc,i*16,0,ILD_NORMAL);
    SelectObject(dc,oldb);
    BITMAP bm; GetObject(bmp,sizeof(bm),&bm);
    BITMAPFILEHEADER bf={0}; BITMAPINFOHEADER bh={0};
    bh.biSize=sizeof(bh);bh.biWidth=bm.bmWidth;bh.biHeight=bm.bmHeight;bh.biPlanes=1;
    bh.biBitCount=24;bh.biCompression=BI_RGB;
    HDC dc2=CreateCompatibleDC(screen); SelectObject(dc2,bmp);
    int sz=((bm.bmWidth*3+3)&~3)*bm.bmHeight;
    unsigned char* buf=new unsigned char[sz];
    GetDIBits(dc2,bmp,0,bm.bmHeight,buf,(BITMAPINFO*)&bh,DIB_RGB_COLORS);
    bf.bfType=0x4D42;bf.bfOffBits=sizeof(bf)+sizeof(bh);bf.bfSize=sizeof(bf)+sizeof(bh)+sz;
    FILE* f=fopen("C:\\icontest.bmp","wb");
    fwrite(&bf,sizeof(bf),1,f);fwrite(&bh,sizeof(bh),1,f);fwrite(buf,sz,1,f);fclose(f);
    printf("bmp written\n");
    return 0;
}
