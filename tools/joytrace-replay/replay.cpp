// Replays a recorded JoyTrace port through JoyBusLagModel; counts what a remote machine would wait for.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include "Core/HW/SI/JoyBusLagModel.h"
using namespace SerialInterface;
using R = JoyBusLagModel::Reply;
struct Cmd { uint64_t t; uint8_t cmd; uint32_t word; R real; };
static R ParseReply(const std::string& s) {
  R r; if (s == "--") { r.len = -1; return r; }
  r.len = (int8_t)(s.size() / 2);
  for (int i = 0; i < r.len && i < 5; i++) r.data[i] = (uint8_t)strtoul(s.substr(2*i, 2).c_str(), nullptr, 16);
  return r;
}
int main(int argc, char** argv) {
  const char* file = argv[1]; const char* port = argv[2]; double lag_ms = argc > 3 ? atof(argv[3]) : 100;
  std::ifstream in(file); std::string line; std::vector<Cmd> cmds;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line); std::string p, tk, c, w, rep, x, last;
    ss >> p >> tk >> c >> w >> rep; if (p != port) continue;
    uint64_t t = strtoull(tk.c_str(), nullptr, 10); uint8_t cmd = (uint8_t)strtoul(c.c_str(), nullptr, 16);
    uint32_t word = w == "-" ? 0 : (uint32_t)strtoul(w.c_str(), nullptr, 16);
    int n = 1; uint64_t lt = t;
    std::string tok;
    R r = ParseReply(rep);
    // A room trace shows what the game heard; "real=" is what the GBA said when that differed.
    while (ss >> tok) {
      if (tok[0] == 'x') { n = atoi(tok.c_str()+1); ss >> last; lt = strtoull(last.c_str(), nullptr, 10); }
      else if (tok.compare(0, 5, "real=") == 0) r = ParseReply(tok.substr(5));
    }
    for (int i = 0; i < n; i++) { uint64_t tt = n > 1 ? t + (lt - t) * i / (n - 1) : t; cmds.push_back({tt, cmd, word, r}); }
  }
  const double tps = 486e6; JoyBusLagModel m((uint64_t)(tps * lag_ms / 1000));
  struct B { long n=0,w=0,l=0,p=0,badl=0,badp=0; long maxrun=0; long wc[3]={0,0,0}; };
  long bestrun=0, bestend=0;
  std::map<int,B> b; long run=0;
  long tot[3]={0,0,0}, bad[2]={0,0}; std::map<std::string,long> hist; long badrecv=0,badother=0;
  for (size_t i = 0; i < cmds.size(); i++) {
    uint32_t k = m.Command(cmds[i].t, cmds[i].cmd, cmds[i].word);
    JoyBusLagModel::How how;
    R heard = m.Decide(k, [&](uint32_t idx){ return cmds[idx].real; }, &how);
    int bucket = (int)(cmds[i].t / tps / 10) * 10; B& x = b[bucket]; x.n++;
    bool same = heard == cmds[i].real;
    if (how == JoyBusLagModel::How::Waited) { x.w++; tot[0]++; run++; x.maxrun = std::max(x.maxrun, run); if(run>bestrun){bestrun=run;bestend=(long)i;} int ci = cmds[i].cmd==0?0:(cmds[i].cmd==0x15?1:2); x.wc[ci]++; if (bucket>=100) { char buf[64]; snprintf(buf,64,"%02x -> %d %02x%02x%02x%02x%02x", cmds[i].cmd, cmds[i].real.len, cmds[i].real.data[0],cmds[i].real.data[1],cmds[i].real.data[2],cmds[i].real.data[3],cmds[i].real.data[4]); hist[buf]++; } }
    else { run = 0; if (how == JoyBusLagModel::How::Lagged) { x.l++; tot[1]++; if(!same){x.badl++;bad[0]++;} } else { x.p++; tot[2]++; if(!same){x.badp++;bad[1]++; bool recv = cmds[i].cmd==0x15 ? (cmds[i].real.data[0]&8) : (cmds[i].real.len>=3 && (cmds[i].real.data[2]&8)); if(recv) badrecv++; else badother++;} } }
  }
  printf("port %s: %zu commands | waited %ld lagged %ld predicted %ld | differs: lagged %ld predicted %ld\n", port, cmds.size(), tot[0], tot[1], tot[2], bad[0], bad[1]);
  printf("  t(s)    cmds  waited lagged predicted  badLag badPred maxWaitRun  waited(status/write/other)\n");
  for (auto& [k, v] : b) printf("  %4d %7ld %7ld %6ld %9ld %7ld %7ld %9ld  %ld/%ld/%ld\n", k, v.n, v.w, v.l, v.p, v.badl, v.badp, v.maxrun, v.wc[0], v.wc[1], v.wc[2]);
  printf("predicted-but-different: real had the receive flag %ld, other %ld\n", badrecv, badother);
  printf("waited after t=100 s, by command and real reply (top):\n"); { std::vector<std::pair<long,std::string>> v; for (auto& [k,c] : hist) v.push_back({c,k}); std::sort(v.rbegin(), v.rend()); for (size_t i=0;i<v.size()&&i<12;i++) printf("   %6ld  %s\n", v[i].first, v[i].second.c_str()); }
  printf("longest wait run: %ld commands ending at t=%.1f s; the last 14 of it and the 6 before:\n", bestrun, cmds[bestend].t/tps);
  for (long i = bestend - bestrun - 5 + 1; i <= bestend - bestrun + 14 + (bestrun>14?0:0) && i < (long)cmds.size(); i++) { if (i<0) continue; printf("   %.3f %02x %08x -> len %d %02x%02x%02x%02x%02x\n", cmds[i].t/tps, cmds[i].cmd, cmds[i].word, cmds[i].real.len, cmds[i].real.data[0], cmds[i].real.data[1], cmds[i].real.data[2], cmds[i].real.data[3], cmds[i].real.data[4]); }
}
