#include <backplane_merge.h>
#include <cassert>
#include <iostream>
using namespace BACKPLANE_MERGE; using nlohmann::json;
int main(){
  // project: person edits clearance of class HS in memory; agent adds class DDR + changes min track on disk
  json base = json::parse(R"({"board":{"design_settings":{"rules":{"min_track_width":0.2,"min_clearance":0.15}}},"net_settings":{"classes":[{"name":"Default","clearance":0.2},{"name":"HS","clearance":0.1}]}})");
  json ours=base; ours["net_settings"]["classes"][1]["clearance"]=0.12;
  json theirs=base; theirs["board"]["design_settings"]["rules"]["min_track_width"]=0.1;
  theirs["net_settings"]["classes"].push_back({{"name","DDR"},{"clearance",0.09}});
  std::vector<CONFLICT> c; json m=MergeJson(base,ours,theirs,c);
  assert(c.empty()); assert(m["net_settings"]["classes"].size()==3);
  assert(m["net_settings"]["classes"][1]["clearance"]==0.12);
  assert(m["board"]["design_settings"]["rules"]["min_track_width"]==0.1);
  // conflict
  json t2=base; t2["net_settings"]["classes"][1]["clearance"]=0.3;
  c.clear(); m=MergeJson(base,ours,t2,c); assert(c.size()==1); std::cout<<c[0].path<<"\n"; assert(m["net_settings"]["classes"][1]["clearance"]==0.12);
  // deletion on one side
  json t3=base; t3["net_settings"]["classes"].erase(1); c.clear(); m=MergeJson(base,base,t3,c); assert(m["net_settings"]["classes"].size()==1);
  // rules
  std::string rb="(version 2)\n\n# clearance for HV\n(rule \"HV\"\n  (constraint clearance (min 1mm)))\n\n(rule \"DDR skew\"\n  (constraint skew (max 5mil)))\n";
  std::string ro=rb; ro.replace(ro.find("1mm"),3,"1.5mm");               // person edits HV
  std::string rt=rb+"\n(rule USB\n  (constraint diff_pair_gap (opt 0.15mm)))\n"; rt.replace(rt.find("5mil"),4,"3mil"); // agent edits DDR, adds USB
  c.clear(); std::string mr=MergeRules(rb,ro,rt,c);
  std::cout<<mr<<"---\n"; assert(c.empty()); assert(mr.find("1.5mm")!=std::string::npos && mr.find("3mil")!=std::string::npos && mr.find("USB")!=std::string::npos && mr.find("# clearance for HV")!=std::string::npos);
  std::string e=EditRules(mr,{"(rule \"HV\"\n  (constraint clearance (min 2mm)))"},{"USB"});
  std::cout<<e; assert(e.find("2mm")!=std::string::npos && e.find("# clearance for HV")!=std::string::npos && e.find("USB")==std::string::npos && e.find("3mil")!=std::string::npos);
  std::cout<<"ALL OK\n";
}
