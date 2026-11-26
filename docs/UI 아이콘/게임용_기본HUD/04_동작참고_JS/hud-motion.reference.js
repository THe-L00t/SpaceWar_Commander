(function () {
  'use strict';
  const clamp=(n,a,b)=>Math.min(b,Math.max(a,n));
  const normalizeAngle=angle=>((angle%360)+360)%360;
  const angleDelta=(angle,yaw)=>normalizeAngle(angle-yaw+180)-180;
  function compassTicks(config){
    const names=['N','NE','E','SE','S','SW','W','NW'],ticks=[];
    for(let angle=0;angle<360;angle+=5){
      const offset=angleDelta(angle,config.yaw)*config.pixelsPerDegree;
      if(Math.abs(offset)>config.width/2+24)continue;
      ticks.push({angle,x:config.centerX+offset,major:angle%15===0,
        label:angle%45===0?names[angle/45]:angle%15===0?String(angle):''});
    }
    return ticks;
  }
  function weaponState(state,runtime={}){
    const h=state.hud,w=state.weapon;
    const canReload=!runtime.reload&&h.ammo<w.magazineSize&&h.reserve>0;
    const low=h.ammo<w.magazineSize&&h.ammo<=Math.floor(w.magazineSize*w.lowAmmoPercent/100);
    const status=runtime.reload?'reloading':h.ammo===0&&h.reserve===0?'empty':low&&h.reserve===0?'no-reserve':low?'low':'ready';
    return {status,canReload,low,promptVisible:h.showReload&&status==='low'&&canReload,
      progress:runtime.reload?clamp(runtime.reload.elapsed/runtime.reload.duration,0,1):0};
  }
  function createRuntime(){return {time:0,paused:false,reload:null};}
  function startReload(state,runtime){
    if(!weaponState(state,runtime).canReload)return false;
    runtime.reload={elapsed:0,duration:state.weapon.reloadDuration,ammo:state.hud.ammo,
      reserve:state.hud.reserve,capacity:state.weapon.magazineSize};
    return true;
  }
  function cancelReload(runtime){const active=Boolean(runtime.reload);runtime.reload=null;return active;}
  function fire(state,runtime){if(runtime.reload||state.hud.ammo<=0)return false;state.hud.ammo--;return true;}
  function advance(state,runtime,seconds){
    if(!Number.isFinite(seconds)||seconds<0)throw new Error('진행 시간은 0 이상의 숫자여야 합니다.');
    if(runtime.paused)return {completed:false,cancelled:false};
    runtime.time+=seconds;
    if(state.compass.autoRotate)state.compass.yaw=normalizeAngle(state.compass.yaw+state.compass.rotationSpeed*seconds);
    const r=runtime.reload;
    if(!r)return {completed:false,cancelled:false};
    if(r.ammo!==state.hud.ammo||r.reserve!==state.hud.reserve||r.capacity!==state.weapon.magazineSize){
      cancelReload(runtime);return {completed:false,cancelled:true};
    }
    r.elapsed=Math.min(r.duration,r.elapsed+seconds);
    if(r.elapsed+1e-9<r.duration)return {completed:false,cancelled:false};
    const transfer=Math.min(r.capacity-state.hud.ammo,state.hud.reserve);
    state.hud.ammo+=transfer;state.hud.reserve-=transfer;runtime.reload=null;
    return {completed:true,cancelled:false};
  }
  window.SW_HUD_MOTION={normalizeAngle,angleDelta,compassTicks,weaponState,createRuntime,startReload,cancelReload,fire,advance};
})();
