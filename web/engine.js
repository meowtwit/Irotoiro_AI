/* ---------- engine (SPEC.md 準拠 / 検証済み参照実装) ---------- */
(function (root, factory) {
  'use strict';
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.Irotoiro = factory();
})(typeof globalThis !== 'undefined' ? globalThis : (typeof window !== 'undefined' ? window : this), function () {
  'use strict';
  const OHA={R:0,B:1,Y:2}, TILE={P:0,G:1,O:2};
  function mixTile(a,b){ const s=a+b; if(s===1)return TILE.P; if(s===3)return TILE.G; return TILE.O; }
  const WINDOWS=(function(){ const ws=[],idx=(r,c)=>r*5+c;
    for(let r=0;r<5;r++)for(let c=0;c<3;c++)ws.push([idx(r,c),idx(r,c+1),idx(r,c+2)]);
    for(let c=0;c<5;c++)for(let r=0;r<3;r++)ws.push([idx(r,c),idx(r+1,c),idx(r+2,c)]);
    for(let r=0;r<3;r++)for(let c=0;c<3;c++)ws.push([idx(r,c),idx(r+1,c+1),idx(r+2,c+2)]);
    for(let r=0;r<3;r++)for(let c=2;c<5;c++)ws.push([idx(r,c),idx(r+1,c-1),idx(r+2,c-2)]);
    return ws; })();
  const WINDOWS_THROUGH=(function(){ const m=Array.from({length:25},()=>[]);
    WINDOWS.forEach((w,wi)=>w.forEach(cell=>m[cell].push(wi))); return m; })();
  function cloneState(s){ return {board:s.board.map(c=>c?{kind:c.kind,color:c.color}:null),stock:s.stock.map(a=>a.slice()),hand:s.hand.map(a=>a.slice()),bag:s.bag.slice(),score:s.score.slice(),toMove:s.toMove}; }
  function initialState(rng){ const s={board:new Array(25).fill(null),stock:[[4,4,4],[4,4,4]],hand:[[0,0,0],[0,0,0]],bag:[8,8,8],score:[0,0],toMove:0};
    for(let p=0;p<2;p++)for(let n=0;n<2;n++)drawOneToHand(s,p,rng); return s; }
  const handCount=(s,p)=>s.hand[p][0]+s.hand[p][1]+s.hand[p][2];
  const bagCount=s=>s.bag[0]+s.bag[1]+s.bag[2];
  const stockCount=(s,p)=>s.stock[p][0]+s.stock[p][1]+s.stock[p][2];
  const isTerminal=s=>stockCount(s,0)===0&&stockCount(s,1)===0;
  function drawOneToHand(s,p,rng){ if(handCount(s,p)>=6)return -1; const total=bagCount(s); if(total===0)return -1;
    let r=Math.floor(rng()*total); for(let t=0;t<3;t++){ if(r<s.bag[t]){ s.bag[t]--; s.hand[p][t]++; return t;} r-=s.bag[t]; } return -1; }
  function legalPlacements(s){ const p=s.toMove,mv=[]; for(let cell=0;cell<25;cell++){ if(s.board[cell]!==null)continue;
    for(let c=0;c<3;c++) if(s.stock[p][c]>0) mv.push({color:c,cell}); } return mv; }
  function applyTurn(s,move,rng){ const p=s.toMove,opp=1-p,ev=[];
    s.board[move.cell]={kind:'oha',color:move.color}; s.stock[p][move.color]--;
    ev.push({type:'place',cell:move.cell,player:p,piece:{kind:'oha',color:move.color}});
    const drawWindows=[],aabWindows=[];
    for(const wi of WINDOWS_THROUGH[move.cell]){ const w=WINDOWS[wi]; const a=s.board[w[0]],b=s.board[w[1]],c=s.board[w[2]];
      if(!a||!b||!c||a.kind!=='oha'||b.kind!=='oha'||c.kind!=='oha')continue;
      const cnt=[0,0,0]; cnt[a.color]++;cnt[b.color]++;cnt[c.color]++; const distinct=cnt.filter(x=>x>0).length;
      if(distinct===1||distinct===3){ drawWindows.push(wi); ev.push({type:'line-oha',window:w.slice(),pattern:distinct===1?'same':'diff'}); }
      else { let mn=-1,mj=-1; for(let col=0;col<3;col++){ if(cnt[col]===1)mn=col; else if(cnt[col]===2)mj=col; }
        const mc=w.find(cell=>s.board[cell].color===mn); aabWindows.push({minorityCell:mc,mixTile:mixTile(mj,mn),minorityColor:mn});
        ev.push({type:'line-oha',window:w.slice(),pattern:'pair'}); } }
    const consumed=new Set(),placedTileCells=[];
    for(const ab of aabWindows){ if(consumed.has(ab.minorityCell))continue; if(s.hand[p][ab.mixTile]<=0)continue;
      s.hand[p][ab.mixTile]--; s.score[p]++; s.board[ab.minorityCell]={kind:'tile',color:ab.mixTile};
      consumed.add(ab.minorityCell); placedTileCells.push(ab.minorityCell);
      ev.push({type:'exchange',cell:ab.minorityCell,player:p,scoredColor:ab.minorityColor,tile:ab.mixTile}); }
    const seen=new Set();
    for(const pc of placedTileCells){ for(const wi of WINDOWS_THROUGH[pc]){ if(seen.has(wi))continue; const w=WINDOWS[wi];
      const a=s.board[w[0]],b=s.board[w[1]],c=s.board[w[2]];
      if(!a||!b||!c||a.kind!=='tile'||b.kind!=='tile'||c.kind!=='tile')continue; seen.add(wi);
      if(s.score[opp]>0){ s.score[opp]--; s.score[p]++; ev.push({type:'steal-point',window:w.slice(),from:opp,to:p}); }
      else if(handCount(s,p)<6&&handCount(s,opp)>0){ let t=-1,best=-1; for(let k=0;k<3;k++) if(s.hand[opp][k]>best){best=s.hand[opp][k];t=k;}
        if(t>=0&&s.hand[opp][t]>0){ s.hand[opp][t]--; s.hand[p][t]++; ev.push({type:'steal-tile',window:w.slice(),from:opp,to:p,tile:t}); } } } }
    for(let k=0;k<drawWindows.length;k++){ const got=[]; for(let n=0;n<3;n++){ const t=drawOneToHand(s,p,rng); if(t<0)break; got.push(t);} if(got.length)ev.push({type:'draw',player:p,tiles:got}); }
    s.toMove=opp; return ev; }
  function randomBotMove(s,rng){ const mv=legalPlacements(s); if(!mv.length)return null; return mv[Math.floor(rng()*mv.length)]; }
  function makeRng(seed){ let a=seed>>>0; return function(){ a|=0; a=(a+0x6D2B79F5)|0; let t=Math.imul(a^(a>>>15),1|a); t=(t+Math.imul(t^(t>>>7),61|t))^t; return ((t^(t>>>14))>>>0)/4294967296; }; }
  function boardSignature(s){ let out=''; const oha='rby',tile='PGO'; for(const c of s.board){ out+=c===null?'.':(c.kind==='oha'?oha[c.color]:tile[c.color]); } return out; }
  return {OHA,TILE,mixTile,initialState,legalPlacements,applyTurn,randomBotMove,makeRng,isTerminal,handCount,bagCount,stockCount,cloneState,WINDOWS,WINDOWS_THROUGH,drawOneToHand,boardSignature};
});
