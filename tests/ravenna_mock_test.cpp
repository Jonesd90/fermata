#include "/home/claude/Fermata/src/core/Ravenna.h"
#include <cstdio>
using namespace td;
static int fails=0;
#define CHECK(c) do{ if(!(c)){++fails; std::printf("FAIL %d: %s\n",__LINE__,#c);} }while(0)
int main(){
    juce::ScopedJuceInitialiser_GUI init;
    // path application
    { auto m = juce::JSON::parse (R"({"id":5,"custom":{"ins":{"channels":[{"m48V":false,"micGain":0},{"m48V":false,"micGain":10}]}}})");
      CHECK(RavennaDevice::applyPath(m,".custom.ins.channels[1]",juce::JSON::parse(R"({"m48V":true})")));
      CHECK((bool)m["custom"]["ins"]["channels"][1]["m48V"] && (int)m["custom"]["ins"]["channels"][1]["micGain"]==10);
      CHECK(RavennaDevice::applyPath(m,".custom.ins.channels",juce::JSON::parse(R"([{"micGain":7}])")));
      CHECK((int)m["custom"]["ins"]["channels"][0]["micGain"]==7 && m["custom"]["ins"]["channels"].size()==1); }
    // framing: a masked frame round trips through the unmasking rule
    { juce::uint8 mask[4]={1,2,3,4}; std::string big(70000,'x'); auto f=WebSocketClient::makeFrame(1,big.data(),big.size(),mask);
      CHECK(f.getSize()==70000+14); CHECK(((juce::uint8*)f.getData())[1]==(0x80|127)); }
    RavennaDevice d("TEST","127.0.0.1",18080);
    d.start();
    for(int i=0;i<60 && d.numChannels()==0;++i) juce::Thread::sleep(100);
    std::printf("status: %s, channels %d\n", d.getStatus().toRawUTF8(), d.numChannels());
    CHECK(d.isConnected()); CHECK(d.numChannels()==2);
    HwPreamp h; CHECK(d.getChannel(1,h)); CHECK(h.micGain==10 && ! h.m48V && h.name=="Combo 1/2");
    // through the driver (local port: use a driver with a custom port is not available, so use the device directly)
    auto* o=new juce::DynamicObject(); o->setProperty("micGain",250); o->setProperty("m48V",true);
    CHECK(d.sendChannelFields(0,juce::var(o)));
    juce::Thread::sleep(600);
    HwPreamp h2; CHECK(d.getChannel(0,h2)); CHECK(h2.micGain==250 && h2.m48V);
    d.stop();
    std::printf(fails? "FAILED\n":"RAVENNA OK\n"); return fails;
}
