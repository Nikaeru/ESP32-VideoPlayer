#pragma once

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>VideoPlayer ESP32</title>
  <style>
    :root{color-scheme:dark;--bg:#111318;--panel:#1b1f27;--line:#343b49;--accent:#8bd5ff;--bad:#ff7b72;--ok:#7ee787}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:#edf2f7;font:15px system-ui,sans-serif}
    main{width:min(760px,calc(100% - 24px));margin:24px auto}.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:18px;margin-bottom:14px}
    h1{font-size:24px;margin:0 0 6px}h2{font-size:17px;margin:0 0 14px}.muted{color:#aab3c2}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
    label{display:block;color:#cbd5e1;margin-bottom:5px}.field{margin-bottom:14px}input,select,button{font:inherit}
    input[type=file],input[type=number],select{width:100%;background:#101319;color:#fff;border:1px solid var(--line);border-radius:8px;padding:9px}
    input[type=range]{width:100%;accent-color:var(--accent)}button{border:0;border-radius:9px;padding:10px 14px;background:#303846;color:#fff;cursor:pointer}
    button.primary{background:#1677a8}button.danger{background:#743a3a}button:disabled{opacity:.45;cursor:not-allowed}.buttons{display:flex;flex-wrap:wrap;gap:8px}
    canvas{display:block;width:100%;max-width:576px;aspect-ratio:72/40;image-rendering:pixelated;background:#000;border:1px solid var(--line);border-radius:8px;margin:auto}
    progress{width:100%;height:18px}.status{padding:10px;border-radius:8px;background:#101319;white-space:pre-wrap}.ok{color:var(--ok)}.bad{color:var(--bad)}
    .decoder{position:fixed;width:1px;height:1px;opacity:.01;pointer-events:none;left:-10px;top:-10px}
    @media(max-width:560px){.grid{grid-template-columns:1fr}main{margin-top:12px}.card{padding:14px}}
  </style>
</head>
<body>
<main>
  <section class="card">
    <h1>VideoPlayer ESP32</h1>
    <div class="muted">MP4 преобразуется на этом устройстве и отправляется на ESP32</div>
  </section>

  <section class="card">
    <h2>1. Выбор фрагмента</h2>
    <div class="field"><label for="file">Видео MP4</label><input id="file" type="file" accept="video/mp4,video/webm,video/*"></div>
    <div class="grid">
      <div><label for="startNumber">Начало, сек</label><input id="startNumber" type="number" min="0" step="0.01" value="0"><input id="startRange" type="range" min="0" max="0" step="0.01" value="0"></div>
      <div><label for="endNumber">Конец, сек</label><input id="endNumber" type="number" min="0" step="0.01" value="0"><input id="endRange" type="range" min="0" max="0" step="0.01" value="0"></div>
    </div>
  </section>

  <section class="card">
    <h2>2. Параметры</h2>
    <div class="grid">
      <div class="field"><label for="fps">FPS: <span id="fpsValue">30</span></label><input id="fps" type="range" min="5" max="40" step="1" value="30"></div>
      <div class="field"><label for="fit">Масштабирование</label><select id="fit"><option value="cover">Заполнить с обрезкой</option><option value="contain">Весь кадр с полями</option><option value="stretch">Растянуть</option></select></div>
      <div class="field"><label for="threshold">Порог: <span id="thresholdValue">128</span></label><input id="threshold" type="range" min="0" max="255" step="1" value="128"></div>
      <div class="field"><label><input id="invert" type="checkbox"> Инвертировать чёрное и белое</label></div>
    </div>
    <canvas id="preview" width="72" height="40"></canvas>
    <p id="estimate" class="status muted">Выберите видео.</p>
    <button id="convert" class="primary" disabled>Преобразовать и загрузить</button>
    <div class="field" style="margin-top:14px"><progress id="progress" max="100" value="0"></progress></div>
    <div id="message" class="status muted">Готово к выбору файла.</div>
  </section>

  <section class="card">
    <h2>3. Управление</h2>
    <div class="field"><label for="speed">Скорость воспроизведения: <span id="speedValue">1.00×</span></label><input id="speed" type="range" min="0.25" max="2" step="0.25" value="1"></div>
    <div class="buttons">
      <button id="play">Play</button><button id="pause">Pause</button><button id="restart">Restart</button><button id="remove" class="danger">Удалить видео</button>
    </div>
    <p id="deviceStatus" class="status muted">Получение состояния ESP32…</p>
  </section>
</main>
<video id="decoder" class="decoder" muted playsinline preload="auto"></video>

<script>
'use strict';
const WIDTH=72,HEIGHT=40,FRAME_BYTES=360,HEADER_BYTES=16;
const $=id=>document.getElementById(id);
const fileInput=$('file'),video=$('decoder'),canvas=$('preview'),ctx=canvas.getContext('2d',{willReadFrequently:true});
const startNumber=$('startNumber'),startRange=$('startRange'),endNumber=$('endNumber'),endRange=$('endRange');
const fpsInput=$('fps'),fitInput=$('fit'),thresholdInput=$('threshold'),invertInput=$('invert');
const speedInput=$('speed');
const convertButton=$('convert'),progress=$('progress'),message=$('message'),estimate=$('estimate'),deviceStatus=$('deviceStatus');
let objectUrl='',maxVideoBytes=0,busy=false,previewToken=0;

function formatBytes(value){if(value<1024)return value+' B';if(value<1048576)return(value/1024).toFixed(1)+' KiB';return(value/1048576).toFixed(2)+' MiB'}
function setMessage(text,type='muted'){message.textContent=text;message.className='status '+type}
function frameCount(){const start=Number(startNumber.value),end=Number(endNumber.value),fps=Number(fpsInput.value);return Math.max(0,Math.floor((end-start)*fps))}
function fileSize(){return HEADER_BYTES+frameCount()*FRAME_BYTES}

function clampTimes(source){
  const duration=Number.isFinite(video.duration)?video.duration:0;
  let start=Math.max(0,Math.min(duration,Number(source.value)||0));
  let end=Math.max(0,Math.min(duration,Number(endNumber.value)||0));
  if(source===endNumber||source===endRange){end=Math.max(start,end)}else{start=Math.min(start,end)}
  startNumber.value=start.toFixed(2);startRange.value=start;
  endNumber.value=end.toFixed(2);endRange.value=end;
  updateEstimate();schedulePreview();
}

function updateEstimate(){
  $('fpsValue').textContent=fpsInput.value;$('thresholdValue').textContent=thresholdInput.value;
  if(!video.src||!Number.isFinite(video.duration)){estimate.textContent='Выберите видео.';convertButton.disabled=true;return}
  const frames=frameCount(),bytes=fileSize(),seconds=Math.max(0,Number(endNumber.value)-Number(startNumber.value));
  const fits=maxVideoBytes>0&&bytes<=maxVideoBytes;
  estimate.textContent=`Длительность: ${seconds.toFixed(2)} сек · Кадров: ${frames} · Файл: ${formatBytes(bytes)} · Лимит: ${formatBytes(maxVideoBytes)}`;
  estimate.className='status '+(fits&&frames>0?'ok':'bad');
  convertButton.disabled=busy||frames<1||!fits;
}

function seekVideo(time){
  const safe=Math.max(0,Math.min(Math.max(0,video.duration-0.001),time));
  if(video.readyState>=2&&Math.abs(video.currentTime-safe)<0.001)return Promise.resolve();
  return new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{cleanup();reject(new Error('Браузер не смог декодировать выбранный кадр.'))},10000);
    const done=()=>{cleanup();resolve()};
    const failed=()=>{cleanup();reject(new Error('Ошибка декодирования видео.'))};
    const cleanup=()=>{clearTimeout(timer);video.removeEventListener('seeked',done);video.removeEventListener('error',failed)};
    video.addEventListener('seeked',done,{once:true});video.addEventListener('error',failed,{once:true});video.currentTime=safe;
  });
}

function drawSource(){
  ctx.fillStyle='#000';ctx.fillRect(0,0,WIDTH,HEIGHT);ctx.imageSmoothingEnabled=true;ctx.imageSmoothingQuality='high';
  const sw=video.videoWidth,sh=video.videoHeight,sourceAspect=sw/sh,targetAspect=WIDTH/HEIGHT;
  if(fitInput.value==='stretch'){ctx.drawImage(video,0,0,WIDTH,HEIGHT);return}
  if(fitInput.value==='cover'){
    let sx=0,sy=0,cw=sw,ch=sh;
    if(sourceAspect>targetAspect){cw=sh*targetAspect;sx=(sw-cw)/2}else{ch=sw/targetAspect;sy=(sh-ch)/2}
    ctx.drawImage(video,sx,sy,cw,ch,0,0,WIDTH,HEIGHT);return;
  }
  let dw=WIDTH,dh=HEIGHT,dx=0,dy=0;
  if(sourceAspect>targetAspect){dh=WIDTH/sourceAspect;dy=(HEIGHT-dh)/2}else{dw=HEIGHT*sourceAspect;dx=(WIDTH-dw)/2}
  ctx.drawImage(video,dx,dy,dw,dh);
}

function makeMonochrome(target=null,targetOffset=0){
  drawSource();const image=ctx.getImageData(0,0,WIDTH,HEIGHT),pixels=image.data;
  const threshold=Number(thresholdInput.value),invert=invertInput.checked;
  for(let y=0;y<HEIGHT;y++)for(let x=0;x<WIDTH;x++){
    const p=(y*WIDTH+x)*4,lum=(pixels[p]*77+pixels[p+1]*150+pixels[p+2]*29)>>8;
    let white=lum>=threshold;if(invert)white=!white;const level=white?255:0;
    pixels[p]=pixels[p+1]=pixels[p+2]=level;pixels[p+3]=255;
    if(target&&white)target[targetOffset+y*9+(x>>3)]|=0x80>>(x&7);
  }
  ctx.putImageData(image,0,0);
}

function median(values){
  const sorted=[...values].sort((a,b)=>a-b),middle=Math.floor(sorted.length/2);
  return sorted.length%2?sorted[middle]:(sorted[middle-1]+sorted[middle])/2;
}

function measureDisplayRefreshRate(){
  return new Promise(resolve=>{
    const intervals=[];let previous=0;
    const sample=time=>{
      if(previous)intervals.push(time-previous);previous=time;
      if(intervals.length<16){requestAnimationFrame(sample);return}
      const measured=1000/median(intervals);
      resolve(measured>=30&&measured<=240?measured:60);
    };
    requestAnimationFrame(sample);
  });
}

async function convertSequentially(output,start,frames,fps,onFramesReady=()=>{},getPipelineError=()=>null){
  if(typeof video.requestVideoFrameCallback!=='function'){
    for(let i=0;i<frames;i++){
      const pipelineError=getPipelineError();if(pipelineError)throw pipelineError;
      await seekVideo(start+i/fps);makeMonochrome(output,HEADER_BYTES+i*FRAME_BYTES);
      onFramesReady(i+1);
    }
    return;
  }

  const displayRate=await measureDisplayRefreshRate();
  await seekVideo(start);video.playbackRate=1;
  setMessage('Определение максимальной скорости без пропуска кадров…');
  const packedFrame=new Uint8Array(FRAME_BYTES);

  await new Promise((resolve,reject)=>{
    let frameIndex=0,callbackId=0,finished=false,lastMediaTime=-1,sourceFps=0;
    const mediaIntervals=[];
    const cleanup=()=>{
      if(callbackId)video.cancelVideoFrameCallback(callbackId);
      video.removeEventListener('error',onError);video.removeEventListener('ended',onEnded);video.pause();video.playbackRate=1;
    };
    const finish=()=>{if(finished)return;finished=true;cleanup();resolve()};
    const fail=error=>{if(finished)return;finished=true;cleanup();reject(error)};
    const fillRemaining=()=>{
      if(frameIndex>=frames)return;
      packedFrame.fill(0);makeMonochrome(packedFrame,0);
      while(frameIndex<frames){output.set(packedFrame,HEADER_BYTES+frameIndex*FRAME_BYTES);frameIndex++}
      onFramesReady(frameIndex);
    };
    const onError=()=>fail(new Error('Ошибка последовательного декодирования видео.'));
    const onEnded=()=>{fillRemaining();finish()};
    const onFrame=(_now,metadata)=>{
      callbackId=0;
      const pipelineError=getPipelineError();if(pipelineError){fail(pipelineError);return}
      if(lastMediaTime>=0){
        const interval=metadata.mediaTime-lastMediaTime;
        if(interval>0.001&&interval<0.2&&mediaIntervals.length<12)mediaIntervals.push(interval);
      }
      lastMediaTime=metadata.mediaTime;

      if(!sourceFps&&mediaIntervals.length>=8){
        sourceFps=1/median(mediaIntervals);
        const requiredUniqueFps=Math.min(sourceFps,fps);
        const safeRate=Math.max(1,Math.min(16,displayRate*0.85/requiredUniqueFps));
        try{video.playbackRate=safeRate}catch(_){video.playbackRate=1}
      }

      const availableIndex=Math.min(frames-1,Math.floor((metadata.mediaTime-start)*fps+0.0001));
      const targetGap=availableIndex-frameIndex;
      if(sourceFps&&fps<=sourceFps*1.05&&targetGap>1&&video.playbackRate>1){
        video.playbackRate=Math.max(1,video.playbackRate*0.8);
      }
      if(availableIndex>=frameIndex){
        packedFrame.fill(0);makeMonochrome(packedFrame,0);
        while(frameIndex<=availableIndex&&frameIndex<frames){output.set(packedFrame,HEADER_BYTES+frameIndex*FRAME_BYTES);frameIndex++}
        onFramesReady(frameIndex);
      }
      if(frameIndex>=frames)finish();else callbackId=video.requestVideoFrameCallback(onFrame);
    };
    video.addEventListener('error',onError,{once:true});video.addEventListener('ended',onEnded,{once:true});
    callbackId=video.requestVideoFrameCallback(onFrame);
    video.play().catch(error=>fail(new Error(`Не удалось запустить декодер: ${error.message}`)));
  });
}

async function schedulePreview(){
  const token=++previewToken;if(busy||!video.src||!Number.isFinite(video.duration))return;
  try{await seekVideo(Number(startNumber.value));if(token!==previewToken||busy)return;makeMonochrome()}catch(_){ }
}

async function refreshStatus(){
  try{
    const response=await fetch('/api/status',{cache:'no-store'}),state=await response.json();maxVideoBytes=state.maxVideoBytes||0;
    speedInput.value=state.speed||1;$('speedValue').textContent=`${Number(state.speed||1).toFixed(2)}×`;
    const playback=state.videoReady?`${state.playing?'Воспроизведение':'Пауза'} · ${state.fps} FPS · ${Number(state.speed||1).toFixed(2)}× · кадр ${state.currentFrame}/${state.frameCount}`:'Видео не загружено';
    deviceStatus.textContent=`${playback}\nLittleFS: ${formatBytes(state.usedBytes)} / ${formatBytes(state.totalBytes)}`;
    deviceStatus.className='status '+(state.videoReady?'ok':'muted');updateEstimate();
  }catch(error){deviceStatus.textContent='Нет связи с ESP32';deviceStatus.className='status bad'}
}

async function uploadRequest(path,body=null){
  const response=await fetch(path,{method:'POST',headers:body?{'Content-Type':'application/x-www-form-urlencoded'}:undefined,body});
  let result={};try{result=await response.json()}catch(_){ }
  if(!response.ok)throw new Error(result.message||`HTTP ${response.status}`);return result;
}

function uploadChunk(blob,onProgress){
  return new Promise((resolve,reject)=>{
    const xhr=new XMLHttpRequest(),form=new FormData();form.append('chunk',blob,'frames.bin');xhr.open('POST','/api/upload/chunk');
    xhr.upload.onprogress=event=>{if(event.lengthComputable)onProgress(Math.min(blob.size,event.loaded))};
    xhr.onerror=()=>reject(new Error('Соединение с ESP32 потеряно.'));
    xhr.onload=()=>{let result={};try{result=JSON.parse(xhr.responseText)}catch(_){ }if(xhr.status>=200&&xhr.status<300)resolve(result);else reject(new Error(result.message||`HTTP ${xhr.status}`))};
    xhr.send(form);
  });
}

async function convertAndUpload(){
  if(busy||convertButton.disabled)return;busy=true;previewToken++;updateEstimate();progress.value=0;video.pause();
  let sessionStarted=false;
  try{
    const fps=Number(fpsInput.value),start=Number(startNumber.value),frames=frameCount(),payload=frames*FRAME_BYTES,total=HEADER_BYTES+payload;
    if(total>maxVideoBytes)throw new Error('Выбранный фрагмент не помещается в LittleFS.');
    const output=new Uint8Array(total),view=new DataView(output.buffer);output.set([66,65,80,49],0);output[4]=1;output[5]=WIDTH;output[6]=HEIGHT;output[7]=fps;view.setUint32(8,frames,true);view.setUint32(12,payload,true);
    await uploadRequest('/api/upload/start',`fps=${fps}&frames=${frames}`);sessionStarted=true;

    const BATCH_FRAMES=32;
    let convertedFrames=0,queuedFrames=0,uploadedBytes=0,uploadFailure=null;
    let uploadChain=Promise.resolve();
    const updatePipeline=(inFlight=0)=>{
      const conversionPart=convertedFrames/frames*70,uploadPart=Math.min(1,(uploadedBytes+inFlight)/payload)*30;
      progress.value=conversionPart+uploadPart;
      setMessage(`Кадры: ${convertedFrames}/${frames} · Отправлено: ${formatBytes(uploadedBytes+inFlight)} / ${formatBytes(payload)}`);
    };
    const enqueueChunk=(fromFrame,toFrame)=>{
      const chunk=output.slice(HEADER_BYTES+fromFrame*FRAME_BYTES,HEADER_BYTES+toFrame*FRAME_BYTES);queuedFrames=toFrame;
      uploadChain=uploadChain.then(async()=>{
        if(uploadFailure)return;
        try{await uploadChunk(new Blob([chunk],{type:'application/octet-stream'}),loaded=>updatePipeline(loaded));uploadedBytes+=chunk.length;updatePipeline()}
        catch(error){uploadFailure=error}
      });
    };
    const framesReady=readyFrames=>{
      convertedFrames=readyFrames;
      while(readyFrames-queuedFrames>=BATCH_FRAMES)enqueueChunk(queuedFrames,queuedFrames+BATCH_FRAMES);
      updatePipeline();
    };

    setMessage('Запуск последовательного декодирования…');
    await convertSequentially(output,start,frames,fps,framesReady,()=>uploadFailure);
    if(queuedFrames<frames)enqueueChunk(queuedFrames,frames);
    await uploadChain;if(uploadFailure)throw uploadFailure;
    const result=await uploadRequest('/api/upload/finish');sessionStarted=false;progress.value=100;setMessage(result.message||'Видео загружено.','ok');await refreshStatus();
  }catch(error){
    if(sessionStarted){try{await uploadRequest('/api/upload/cancel')}catch(_){ }}
    setMessage(error.message||String(error),'bad');
  }
  finally{busy=false;updateEstimate()}
}

async function control(path,method='POST',body=null){
  try{const response=await fetch(path,{method,headers:body?{'Content-Type':'application/x-www-form-urlencoded'}:undefined,body}),result=await response.json();if(!response.ok)throw new Error(result.message||`HTTP ${response.status}`);setMessage(result.message,'ok');await refreshStatus()}
  catch(error){setMessage(error.message||String(error),'bad')}
}

fileInput.addEventListener('change',()=>{
  if(objectUrl)URL.revokeObjectURL(objectUrl);const file=fileInput.files[0];if(!file)return;
  objectUrl=URL.createObjectURL(file);video.src=objectUrl;video.load();setMessage('Чтение метаданных видео…');
});
video.addEventListener('loadedmetadata',()=>{
  const duration=video.duration;startNumber.max=startRange.max=endNumber.max=endRange.max=duration;startNumber.value=startRange.value=0;endNumber.value=endRange.value=duration.toFixed(2);endRange.value=duration;
  setMessage(`Видео открыто: ${duration.toFixed(2)} сек.`,'ok');updateEstimate();schedulePreview();
});
video.addEventListener('error',()=>setMessage('Браузер не поддерживает кодек этого видео.','bad'));
startNumber.addEventListener('change',()=>clampTimes(startNumber));startRange.addEventListener('input',()=>{startNumber.value=startRange.value;clampTimes(startRange)});
endNumber.addEventListener('change',()=>clampTimes(endNumber));endRange.addEventListener('input',()=>{endNumber.value=endRange.value;clampTimes(endRange)});
[fpsInput,thresholdInput].forEach(input=>input.addEventListener('input',()=>{updateEstimate();schedulePreview()}));
[fitInput,invertInput].forEach(input=>input.addEventListener('change',()=>{updateEstimate();schedulePreview()}));
convertButton.addEventListener('click',convertAndUpload);$('play').addEventListener('click',()=>control('/api/play'));$('pause').addEventListener('click',()=>control('/api/pause'));$('restart').addEventListener('click',()=>control('/api/restart'));
$('speed').addEventListener('change',()=>control('/api/speed','POST',`value=${encodeURIComponent(speedInput.value)}`));
$('speed').addEventListener('input',()=>$('speedValue').textContent=`${Number(speedInput.value).toFixed(2)}×`);
$('remove').addEventListener('click',()=>{if(confirm('Удалить сохранённое видео?'))control('/api/video','DELETE')});
refreshStatus();setInterval(()=>{if(!busy)refreshStatus()},2000);updateEstimate();
</script>
</body>
</html>
)HTML";
