#include "character_preview.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace anim {
namespace {
constexpr float pi = 3.14159265359f;
struct Point { float x, y; };
struct Matrix {
    float a=1, b=0, c=0, d=1, x=0, y=0;
    Point Map(Point p) const { return {a*p.x+c*p.y+x, b*p.x+d*p.y+y}; }
    Matrix At(float tx, float ty, float angle=0, float scale=1) const {
        const float co=std::cos(angle)*scale, si=std::sin(angle)*scale;
        const Point p=Map({tx,ty});
        return {a*co+c*si,b*co+d*si,-a*si+c*co,-b*si+d*co,p.x,p.y};
    }
};
uint16_t Color(uint32_t rgb) {
    const uint16_t c=((rgb>>8)&0xf800)|((rgb>>5)&0x7e0)|((rgb>>3)&0x1f);
    return (c<<8)|(c>>8);
}
float Ramp(float v) { v=std::clamp(v,0.f,1.f); return v*v*(3-2*v); }
constexpr uint32_t ivory=0xfff5db, wood=0xedbb79, neck=0x957254, dark=0x294b3d;
class Painter {
    uint16_t* pixels_;
    std::array<Point,256> points_{};
    size_t count_=0;
    Matrix matrix_;
    Point last_{};
public:
    explicit Painter(uint8_t* buffer, bool clear=true):pixels_(reinterpret_cast<uint16_t*>(buffer)) {
        if(!clear)return;
        std::fill(pixels_,pixels_+360*360,Color(0x081312));
        std::memset(buffer+360*360*2,255,360*360);
    }
    void Begin(Matrix m,float x,float y) { matrix_=m;count_=0;To(x,y); }
    void To(float x,float y) { last_={x,y};if(count_<points_.size())points_[count_++]=matrix_.Map(last_); }
    void Curve(float ax,float ay,float bx,float by,float x,float y) {
        const Point p=last_;
        for(int i=1;i<=12;i++){float t=i/12.f,u=1-t;To(u*u*u*p.x+3*u*u*t*ax+3*u*t*t*bx+t*t*t*x,u*u*u*p.y+3*u*u*t*ay+3*u*t*t*by+t*t*t*y);}
    }
    void Fill(uint32_t rgb) {
        if(count_<3)return;
        float lo=360,hi=0;for(size_t i=0;i<count_;i++){lo=std::min(lo,points_[i].y);hi=std::max(hi,points_[i].y);}
        // Compute each edge's slope once, not twice per scanline. Division is
        // expensive on the ESP32, especially for the many guitar contours.
        struct Edge { float x, y, end_y, slope; };
        std::array<Edge,256> edges;
        size_t edge_count=0;
        for(size_t i=0,j=count_-1;i<count_;j=i++) {
            auto p=points_[i],q=points_[j];
            if(p.y==q.y)continue;
            if(p.y>q.y)std::swap(p,q);
            edges[edge_count++]={p.x,p.y,q.y,(q.x-p.x)/(q.y-p.y)};
        }
        const uint16_t color=Color(rgb);
        for(int y=std::max(0,int(std::floor(lo)));y<std::min(360,int(std::ceil(hi)));y++){
            // Two vertical coverage samples and fractional horizontal coverage
            // soften the 360px contours without a 720px supersample frame.
            std::array<float,360> coverage{};
            int row_left=360,row_right=0;
            for(float offset:{.25f,.75f}) {
                std::array<float,256> intersections;size_t n=0;
                const float scan=y+offset;
                for(size_t i=0;i<edge_count;i++){
                    const auto& e=edges[i];
                    if(e.y<=scan&&e.end_y>scan)
                        intersections[n++]=e.x+(scan-e.y)*e.slope;
                }
                std::sort(intersections.begin(),intersections.begin()+n);
                for(size_t i=0;i+1<n;i+=2){
                    int left=std::max(0,int(std::floor(intersections[i])));
                    int right=std::min(360,int(std::ceil(intersections[i+1])));
                    row_left=std::min(row_left,left);row_right=std::max(row_right,right);
                    // Interior pixels have exactly half coverage for this
                    // sample; only the two boundary pixels need clipping.
                    for(int x=left+1;x<right-1;x++)coverage[x]+=.5f;
                    if(left<right)coverage[left]+=.5f*std::max(0.f,std::min(float(left+1),intersections[i+1])-std::max(float(left),intersections[i]));
                    if(left+1<right)coverage[right-1]+=.5f*std::max(0.f,std::min(float(right),intersections[i+1])-std::max(float(right-1),intersections[i]));
                }
            }
            for(int x=row_left;x<row_right;x++) {
                const unsigned alpha=std::min(256u,static_cast<unsigned>(coverage[x]*256));
                auto& dst=pixels_[y*360+x];
                if(alpha==256){dst=color;continue;}
                if(alpha==0)continue;
                const uint16_t a=(color<<8)|(color>>8),b=(dst<<8)|(dst>>8);
                const unsigned r=(((a>>11)&31)*alpha+((b>>11)&31)*(256-alpha))>>8;
                const unsigned g=(((a>>5)&63)*alpha+((b>>5)&63)*(256-alpha))>>8;
                const unsigned bl=((a&31)*alpha+(b&31)*(256-alpha))>>8;
                const uint16_t mixed=(r<<11)|(g<<5)|bl;
                dst=(mixed<<8)|(mixed>>8);
            }
        }
    }
    void Oval(Matrix m,float x,float y,float rx,float ry,uint32_t color){
        Begin(m,x+rx,y);for(int i=1;i<48;i++){float a=i*2*pi/48;To(x+rx*std::cos(a),y+ry*std::sin(a));}Fill(color);
    }
    void Line(Matrix m,float x,float y,float xx,float yy,float width,uint32_t color){
        const float length=std::hypot(xx-x,yy-y);if(length<.001f)return;
        const float dx=-(yy-y)/length*width/2,dy=(xx-x)/length*width/2;
        Begin(m,x+dx,y+dy);To(xx+dx,yy+dy);To(xx-dx,yy-dy);To(x-dx,y-dy);Fill(color);
    }
    void Paw(Matrix m,bool grip=false){
        if(grip){Begin(m,-27,-12);Curve(-22,-25,-5,-28,3,-23);Curve(12,-34,28,-25,25,-15);Curve(37,-13,37,0,29,6);Curve(37,17,22,31,2,31);Curve(-21,32,-35,8,-27,-12);}
        else{Begin(m,-31,-8);Curve(-37,-28,-19,-36,-13,-22);Curve(-16,-44,9,-45,12,-25);Curve(23,-41,38,-29,32,-12);Curve(43,21,19,39,-1,37);Curve(-24,35,-32,15,-31,-8);}
        Fill(ivory);
    }
    void Eye(Matrix m,bool happy,float blink){
        if(happy){Begin(m,0,-40.5f);Curve(22,-40.5f,43.12f,-15.3f,44,9);Curve(44,13.5f,42.24f,14.4f,39.6f,11.7f);Curve(26.4f,-4.5f,17.6f,-20.7f,0,-20.7f);Curve(-17.6f,-20.7f,-26.4f,-4.5f,-39.6f,11.7f);Curve(-42.24f,14.4f,-44,13.5f,-44,9);Curve(-43.12f,-15.3f,-22,-40.5f,0,-40.5f);}
        else{const float k=.55228475f;Begin(m,0,-79*blink);Curve(44*k,-79*blink,44,(-35-44*k)*blink,44,-35*blink);To(44,35*blink);Curve(44,(35+44*k)*blink,44*k,79*blink,0,79*blink);Curve(-44*k,79*blink,-44,(35+44*k)*blink,-44,35*blink);To(-44,-35*blink);Curve(-44,(-35-44*k)*blink,-44*k,-79*blink,0,-79*blink);}
        Fill(ivory);
    }
};
void DrawStrummingPaw(Painter& p, const Matrix& g, float seconds) {
    const float t=std::fmod(seconds,1.05f)/1.05f,reach=22.4f;
    const float u=Ramp(t<.32f?t/.32f:(t-.32f)/.68f);
    const float x=t<.32f?-54:-54-16*std::sin(u*pi);
    const float y=t<.32f?-reach+2*reach*u:reach-2*reach*u;
    const float angle=pi/4+.49f+(t<.32f?-.12f+.24f*u:.12f-.24f*u);
    p.Paw(g.At(x,y,angle,.76f));
}
bool RenderFrame(uint8_t* buffer,size_t size,CharacterPreview scene,float seconds,bool base_only=false){
    if(!buffer||size<kCharacterBytes||!std::isfinite(seconds)||seconds<0)return false;
    Painter p(buffer);const Matrix screen{.5f,0,0,.5f,0,0};
    const bool happy=scene!=CharacterPreview::kEyes;
    const float phase=std::fmod(seconds,3.5f), blink=phase<.22f?1-.94f*std::sin(phase/.22f*pi):1;
    for(int side:{-1,1})p.Eye(screen.At(360+side*118,348+(happy?-7:0)),happy,blink);
    if(scene==CharacterPreview::kWave){
        p.Paw(screen.At(524,420,.12f+std::sin(seconds*5)*.165f));
    }else if(scene==CharacterPreview::kGuitar){
        const auto g=screen.At(340,480,-.49f);
        p.Begin(g,49,-14);p.Curve(47,-47,24,-65,4,-61);p.Curve(-19,-60,-20,-52,-42,-54);p.Curve(-63,-56,-66,-80,-104,-81);p.Curve(-143,-83,-164,-48,-164,0);p.Curve(-164,48,-143,83,-104,81);p.Curve(-66,80,-63,56,-42,54);p.Curve(-20,52,-19,60,4,61);p.Curve(24,65,47,47,49,14);p.Fill(wood);
        p.Begin(g,17,-13);p.Curve(26,-15,39,-14,48,-13);p.To(185,-10);p.To(185,10);p.To(48,13);p.Curve(39,14,26,15,17,13);p.Fill(neck);
        p.Line(g,188,0,218,0,38,neck);
        for(float x:{190.f,213.f})for(float y:{-24.f,24.f})p.Oval(g,x,y,4.5f,4.5f,ivory);
        p.Oval(g,-24,0,34,34,neck);p.Oval(g,-24,0,32,32,wood);p.Oval(g,-24,0,30,30,dark);
        p.Line(g,-105,-24,-105,24,14,0x705d47);
        for(float x:{52.f,80.f,106.f,130.f,152.f,173.f})p.Line(g,x,-11,x,11,1.5f,0xd8c6a6);
        for(float y:{-7.f,0.f,7.f})p.Line(g,-105,y,212,y,1.3f,ivory);
        p.Paw(g.At(103,7,-.2f,.73f),true);
        if(!base_only)DrawStrummingPaw(p,g,seconds);
    }
    return true;
}
} // namespace
bool RenderCharacterPreview(uint8_t* buffer,size_t size,CharacterPreview scene,float seconds) {
    return RenderFrame(buffer,size,scene,seconds);
}
bool RenderGuitarBase(uint8_t* buffer,size_t size) {
    return RenderFrame(buffer,size,CharacterPreview::kGuitar,0,true);
}
bool RenderCachedGuitar(uint8_t* buffer,size_t size,const uint8_t* base,size_t base_size,float seconds) {
    if(!buffer||!base||size<kCharacterBytes||base_size<kCharacterBytes||
       !std::isfinite(seconds)||seconds<0)return false;
    const auto dst=reinterpret_cast<uintptr_t>(buffer),src=reinterpret_cast<uintptr_t>(base);
    if((dst>=src ? dst-src : src-dst)<kCharacterBytes)return false;
    // Restore immutable pixels before compositing: no trails, and no mutation
    // of the currently displayed front buffer or the cached background.
    std::memcpy(buffer,base,kCharacterBytes);
    Painter p(buffer,false);
    const Matrix screen{.5f,0,0,.5f,0,0};
    DrawStrummingPaw(p,screen.At(340,480,-.49f),seconds);
    return true;
}
} // namespace anim
