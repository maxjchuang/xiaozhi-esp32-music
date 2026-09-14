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
    Matrix Scale(float sx,float sy) const { return {a*sx,b*sx,c*sy,d*sy,x,y}; }
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
    unsigned opacity_ = 256;
public:
    void Opacity(float value) { opacity_ = static_cast<unsigned>(std::clamp(value,0.f,1.f)*256); }
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
        if(count_<3||opacity_==0)return;
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
                const unsigned alpha=(std::min(256u,static_cast<unsigned>(coverage[x]*256))*opacity_)>>8;
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
    void Arc(Matrix m,float x,float y,float radius,float start,float end,float width,uint32_t color) {
        const float outer=radius+width/2,inner=std::max(0.f,radius-width/2);
        Begin(m,x+outer*std::cos(start),y+outer*std::sin(start));
        for(int i=1;i<=48;i++){const float a=start+(end-start)*i/48;To(x+outer*std::cos(a),y+outer*std::sin(a));}
        for(int i=48;i>=0;i--){const float a=start+(end-start)*i/48;To(x+inner*std::cos(a),y+inner*std::sin(a));}
        Fill(color);
    }
    void Paw(Matrix m,bool grip=false){
        if(grip){Begin(m,-27,-12);Curve(-22,-25,-5,-28,3,-23);Curve(12,-34,28,-25,25,-15);Curve(37,-13,37,0,29,6);Curve(37,17,22,31,2,31);Curve(-21,32,-35,8,-27,-12);}
        else{Begin(m,-31,-8);Curve(-37,-28,-19,-36,-13,-22);Curve(-16,-44,9,-45,12,-25);Curve(23,-41,38,-29,32,-12);Curve(43,21,19,39,-1,37);Curve(-24,35,-32,15,-31,-8);}
        Fill(ivory);
    }
    void RestPaw(Matrix m) {
        Begin(m,-35,-2);Curve(-43,-11,-34,-18,-24,-12);Curve(-29,-24,-14,-28,-6,-17);
        Curve(-8,-29,9,-26,15,-16);Curve(43,-16,47,9,27,20);Curve(5,32,-29,24,-35,-2);Fill(ivory);
    }
    void CupPaw(Matrix m) {
        Begin(m,-32,-5);Curve(-37,-20,-20,-27,-15,-17);Curve(-12,-30,5,-29,9,-17);
        Curve(19,-28,33,-20,32,-5);Curve(34,17,17,29,-1,29);Curve(-19,29,-31,15,-32,-5);Fill(ivory);
    }
    void Rect(Matrix m,float x,float y,float width,float height,float radius,uint32_t color) {
        if(radius<=0){Begin(m,x,y);To(x+width,y);To(x+width,y+height);To(x,y+height);Fill(color);return;}
        const float k=.55228475f,r=radius;
        Begin(m,x+r,y);To(x+width-r,y);Curve(x+width-r+k*r,y,x+width,y+r-k*r,x+width,y+r);
        To(x+width,y+height-r);Curve(x+width,y+height-r+k*r,x+width-r+k*r,y+height,x+width-r,y+height);
        To(x+r,y+height);Curve(x+r-k*r,y+height,x,y+height-r+k*r,x,y+height-r);
        To(x,y+r);Curve(x,y+r-k*r,x+r-k*r,y,x+r,y);Fill(color);
    }
    void Eye(Matrix m,bool happy,float blink){
        if(happy){Begin(m,0,-40.5f);Curve(22,-40.5f,43.12f,-15.3f,44,9);Curve(44,13.5f,42.24f,14.4f,39.6f,11.7f);Curve(26.4f,-4.5f,17.6f,-20.7f,0,-20.7f);Curve(-17.6f,-20.7f,-26.4f,-4.5f,-39.6f,11.7f);Curve(-42.24f,14.4f,-44,13.5f,-44,9);Curve(-43.12f,-15.3f,-22,-40.5f,0,-40.5f);}
        else{const float k=.55228475f;Begin(m,0,-79*blink);Curve(44*k,-79*blink,44,(-35-44*k)*blink,44,-35*blink);To(44,35*blink);Curve(44,(35+44*k)*blink,44*k,79*blink,0,79*blink);Curve(-44*k,79*blink,-44,(35+44*k)*blink,-44,35*blink);To(-44,-35*blink);Curve(-44,(-35-44*k)*blink,-44*k,-79*blink,0,-79*blink);}
        Fill(ivory);
    }
    void EyeMood(Matrix m,float smile,float height) {
        if(smile<=0){Eye(m.Scale(1,height/158),false,1);return;}
        if(smile>=1){Eye(m.Scale(1,height/90),true,1);return;}
        constexpr float cap=44.f/158, shoulder=.5f-cap,k=.55228475f;
        const float open[]={0,-.5f,.5f*k,-.5f,.5f,-shoulder-cap*k,.5f,-shoulder,
            .5f,-shoulder/3,.5f,shoulder/3,.5f,shoulder,
            .5f,shoulder+cap*k,.5f*k,.5f,0,.5f,
            -.5f*k,.5f,-.5f,shoulder+cap*k,-.5f,shoulder,
            -.5f,shoulder/3,-.5f,-shoulder/3,-.5f,-shoulder,
            -.5f,-shoulder-cap*k,-.5f*k,-.5f,0,-.5f};
        const float happy[]={0,-.45f,.25f,-.45f,.49f,-.17f,.5f,.1f,
            .5f,.15f,.48f,.16f,.45f,.13f,
            .3f,-.05f,.2f,-.23f,0,-.23f,
            -.2f,-.23f,-.3f,-.05f,-.45f,.13f,
            -.48f,.16f,-.5f,.15f,-.5f,.1f,
            -.49f,-.17f,-.25f,-.45f,0,-.45f};
        float points[38];
        for(unsigned i=0;i<38;i++)points[i]=(open[i]+(happy[i]-open[i])*smile)*(i%2?height:88);
        Begin(m,points[0],points[1]);
        for(unsigned i=2;i<38;i+=6)Curve(points[i],points[i+1],points[i+2],points[i+3],points[i+4],points[i+5]);
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
void DrawTheatre(Painter& p, const Matrix& screen, CharacterPreview scene, float age) {
    const float duration=CharacterPreviewDurationMs(scene)/1000.f;
    const float presence=Ramp(age/.65f)*Ramp((duration-age)/.65f);
    const float lift=(1-presence)*95;
    const float swim=Ramp((age-.4f)/3.5f),escape=Ramp((age-4)/1.6f);
    const float fish_x=190+300*swim+170*escape;
    const float fish_y=478-18*std::sin(swim*pi)-55*escape;
    const float pounce=Ramp((age-2.8f)/.25f)*(1-Ramp((age-3.4f)/.55f));
    const float fly=Ramp((age-3.1f)/2.25f);
    float x=0,y=0,height=158,asym=0;
    if(scene==CharacterPreview::kFish) {
        x=std::clamp((fish_x-360)*.2f,-35.f,35.f)*presence;
        y=12*presence;height-=24*pounce;
    } else {
        // Deterministic easing replaces the browser's frame-history smoothing
        // so repeated renders at the same timestamp are identical.
        x=(12+fly*9)*presence*(1-Ramp((age-5.8f)/.4f));y=-fly*19*presence;
        height-=24*presence*Ramp((age-1)/.14f)*(1-Ramp((age-3.1f)/.14f));
        const float surprise=Ramp((age-5.35f)/.08f)*(1-Ramp((age-5.8f)/.12f));
        height+=22*surprise;asym=8*surprise;
    }
    const float yaw=std::clamp(x/48,-1.f,1.f),turn=std::abs(yaw);
    // Eye axes remain vertical; coupled spacing/size supplies the side glance.
    for(int side:{-1,1}) {
        const bool far=side*yaw>0;
        auto eye=screen.At(360+x+side*118*(1-.13f*turn),348+y+side*asym);
        eye.a*=1-(far?.14f:.025f)*turn;
        eye.d*=(height/158)*(1-(far?.035f:0)*turn);
        p.Eye(eye,false,1);
    }
    if(scene==CharacterPreview::kFish) {
        const auto fish=screen.At(fish_x,fish_y,std::sin(age*5)*.05f);
        p.Opacity(presence*(1-escape));
        p.Begin(fish,-21,0);p.To(-38,-15);
        p.Curve(-34,-5,-34,5,-38,15);p.Fill(0xb5cdaa);
        p.Oval(fish,0,0,25,13,0xb5cdaa);p.Oval(fish,12,-3,2.5f,2.5f,0x081312);
        p.Opacity(presence);
        p.Paw(screen.At(400+pounce*57,548+lift-pounce*76,-.35f*pounce));
    } else {
        const auto m=screen.At(-62,0);
        const float withdraw=Ramp((age-3.3f)/.7f),stick_y=465+lift+withdraw*70;
        p.Opacity(presence*(1-withdraw));
        p.Line(m,422,stick_y+35,422,stick_y-59,8,0xbdcfac);
        p.Arc(m,422,stick_y-78,19,0,2*pi,5,0xbdcfac);
        p.Paw(m.At(422,stick_y+8),true);
        if(age>1&&age<5.35f) {
            const float grow=Ramp((age-1)/2.1f),bx=422+fly*42,by=387-fly*173,radius=8+grow*43;
            p.Opacity(presence*(24/255.f));p.Oval(m,bx,by,radius,radius,0xb7dcca);
            p.Opacity(presence);p.Arc(m,bx,by,radius,0,2*pi,2.5f,0xbfdfd5);
            p.Opacity(presence*(160/255.f));p.Arc(m,bx,by,radius*.77f,3.5f,4.6f,2.5f,ivory);
        }
        if(age>=5.35f&&age<5.8f) {
            const float burst=(age-5.35f)/.45f;
            p.Opacity(presence*(1-burst));
            for(int i=0;i<7;i++) {
                const float a=i*2*pi/7;
                p.Line(m,464+std::cos(a)*(50+burst*15),214+std::sin(a)*(50+burst*15),
                       464+std::cos(a)*(56+burst*35),214+std::sin(a)*(56+burst*35),3,0xbfdfd5);
            }
        }
    }
    p.Opacity(1);
}
void DrawNote(Painter& p,Matrix screen,float x,float y,float opacity,float angle) {
    p.Opacity(opacity);
    const auto m=screen.At(x,y,angle);
    p.Oval(m,0,0,8,5,0xbfdbc0);p.Line(m,6,0,6,-26,4,0xbfdbc0);p.Line(m,6,-26,17,-20,4,0xbfdbc0);
}

void DrawRemaining(Painter& p,const Matrix& screen,CharacterPreview scene,float age) {
    constexpr float amount=.55f;
    const float duration=CharacterPreviewDurationMs(scene)/1000.f;
    const float presence=Ramp(age/.65f)*Ramp((duration-age)/.65f),lift=(1-presence)*95;
    const float chin_lift=Ramp((age-.15f)/.7f)*(1-Ramp((age-4.1f)/.9f));
    const float lean=Ramp((age-1)/.8f)*(1-Ramp((age-3.1f)/.8f));
    const float rub_lift=Ramp((age-.65f)/.7f)*(1-Ramp((age-3.5f)/.9f));
    const float close=Ramp(age/.6f);
    const float cover=Ramp((age-.3f)/.7f)*(1-Ramp((age-3.8f)/.65f));
    const float peek=Ramp((age-2)/.35f)*(1-Ramp((age-3.1f)/.35f));
    const float joy=Ramp((age-3.9f)/.6f)*Ramp((6-age)/.6f);
    float x=0,y=0,height=158,asym=0,roll=0,smile=0;
    switch(scene) {
    case CharacterPreview::kChin: x=32*lean;y=8*lean;height-=8*lean;asym=-5*lean;roll=.14f*lean;break;
    case CharacterPreview::kRub: height+=(25-158)*close;y=9*close;break;
    case CharacterPreview::kPeek: height+=(32-158)*cover+(90-158)*joy;smile=joy;asym=35*peek;break;
    case CharacterPreview::kHeart: smile=presence;height+=(90-158)*presence;y=-9*presence;break;
    default: smile=presence;height+=(90-158)*presence;y=(-7+std::sin(age*5)*2*amount)*presence;break;
    }
    const float yaw=std::clamp(x/48,-1.f,1.f),turn=std::abs(yaw);
    const auto face=screen.At(360+x,348+y,roll);
    for(int side:{-1,1}) {
        const bool far=side*yaw>0;
        auto eye=face.At(side*118*(1-.13f*turn),side*asym*.25f);
        eye=eye.Scale(1-(far?.14f:.025f)*turn,1);
        p.EyeMood(eye,smile,std::max(6.f,(height+side*asym)*(1-(far?.035f:0)*turn)));
    }
    p.Opacity(presence);
    switch(scene) {
    case CharacterPreview::kChin: {
        p.Opacity(chin_lift);
        auto hand=screen.At(520+24*(1-chin_lift),502+64*(1-chin_lift)).Scale(1+lean*.04f,1-lean*.06f);
        p.RestPaw(hand.At(0,0,.06f).Scale(-1,1));
        break;
    }
    case CharacterPreview::kRub: {
        const float phase=std::clamp((age-1.5f)/1.6f,0.f,1.f)*pi*4;
        const bool rubbing=age>=1.5f&&age<=3.1f;
        const float dx=rubbing?std::sin(phase)*5*amount:0,dy=rubbing?(1-std::cos(phase))*3*amount:0;
        p.Opacity(rub_lift);
        p.Paw(screen.At(484+20*(1-rub_lift)+dx,362+140*(1-rub_lift)+dy).Scale(.8f,.8f).At(0,0,.12f).Scale(-1,1),true);
        break;
    }
    case CharacterPreview::kPeek:
        for(int side:{-1,1}) {
            const float opening=side==1?peek*62:0;
            p.Paw(screen.At(360+side*(118+opening),520-172*cover+lift,side*(.08f+.22f*peek),1.65f).Scale(-side,1));
        }
        break;
    case CharacterPreview::kHeart: {
        const float beat=1+std::sin(age*3)*.025f*amount;
        const auto m=screen.At(360,459+lift,0,beat);
        p.Begin(m,0,35);p.Curve(-93,-12,-34,-64,0,-24);p.Curve(34,-64,93,-12,0,35);p.Fill(0xdfa797);
        p.CupPaw(screen.At(321,486+lift,.20f));p.CupPaw(screen.At(399,486+lift,-.20f).Scale(-1,1));
        p.Opacity(.22f*presence);
        for(int side:{-1,1})p.Oval(screen,360+side*139+x,411+y,21,7,0xeda88d);
        break;
    }
    case CharacterPreview::kShaker:
        for(int side:{-1,1}) {
            p.Opacity(presence);
            const float swing=std::sin(age*7+side*pi/2)*.28f*amount;
            const auto m=screen.At(360+side*137,481+lift,side*.20f+swing);
            p.Line(m,0,44,0,-51,12,0xc6aa7d);p.Oval(m,0,-67,25,34,side==1?0xb5cdaa:0xdec095);
            p.Line(m,-20,-68,20,-68,5,ivory);p.Paw(m.At(0,13).Scale(-side,1),true);
            const float phase=std::fmod(age+side*.55f,1.9f)/1.9f;
            DrawNote(p,screen,360+side*(192+phase*12),355-phase*103,presence*std::max(0.f,std::sin(phase*pi)),side*.15f);
        }
        break;
    case CharacterPreview::kDrum: {
        const auto m=screen.At(360,493+lift);
        p.Rect(m,-98,0,196,65,0,0xbd9278);p.Oval(m,0,65,98,23,0xbd9278);
        for(int xline=-75;xline<90;xline+=50)p.Line(m,xline,8,xline+20,58,4,0xf0d8ad);
        p.Oval(m,0,0,100,30,0xeddfbe);
        for(int side:{-1,1}) {
            const float beat=std::fmod(age*1.6f+(side==1?.5f:0),1.f);
            const float down=std::pow(std::sin(beat*pi),6),hy=-70+down*53;
            p.Line(m,side*91,hy,side*43,hy+24,9,0xc6aa7d);p.Oval(m,side*43,hy+24,9,7,ivory);
            p.Paw(m.At(side*94,hy-2,side*.2f).Scale(-side,1),true);
        }
        const float phase=std::fmod(age,2.f)/2;
        DrawNote(p,screen,533,410-phase*60,presence*std::sin(phase*pi),.1f);
        break;
    }
    case CharacterPreview::kKeys: {
        const auto m=screen.At(360,503+lift);
        p.Rect(m,-132,-17,264,88,18,0x94b399);
        for(int i=0;i<8;i++) {
            const bool pressed=i==static_cast<int>(age*3)%8;
            p.Rect(m,-120+i*30,-5+(pressed?4:0),27,59-(pressed?4:0),0,pressed?0xd4dfb7:ivory);
        }
        for(int i:{0,1,3,4,5})p.Rect(m,-101+i*30,-5,15,31,0,dark);
        for(int side:{-1,1}) {
            const float bounce=(1-std::cos(age*6+side*pi/2))*5;
            p.Paw(m.At(side*(58+std::sin(age*1.4f)*12),-19+bounce).Scale(.78f,.78f).At(0,0,pi).Scale(side,1));
        }
        const float phase=std::fmod(age,2.f)/2;
        DrawNote(p,screen,543,417-phase*55,presence*std::sin(phase*pi),.15f);
        break;
    }
    default: break;
    }
    p.Opacity(1);
}

bool RenderFrame(uint8_t* buffer,size_t size,CharacterPreview scene,float seconds,bool base_only=false){
    if(!buffer||size<kCharacterBytes||!std::isfinite(seconds)||seconds<0)return false;
    if(scene<CharacterPreview::kEyes||scene>CharacterPreview::kKeys)return false;
    Painter p(buffer);const Matrix screen{.5f,0,0,.5f,0,0};
    if(scene>=CharacterPreview::kChin) {
        DrawRemaining(p,screen,scene,std::min(seconds,CharacterPreviewDurationMs(scene)/1000.f));
        return true;
    }
    if(scene==CharacterPreview::kBubble||scene==CharacterPreview::kFish) {
        DrawTheatre(p,screen,scene,std::min(seconds,CharacterPreviewDurationMs(scene)/1000.f));
        return true;
    }
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
