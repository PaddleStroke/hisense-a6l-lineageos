"""Decode copied A6L media read-only; write separate metrics and audio previews."""
from pathlib import Path
import argparse,hashlib,json,math,wave
import av,numpy as np
from PIL import Image,ImageOps,ImageDraw

parser=argparse.ArgumentParser()
parser.add_argument('media',type=Path)
parser.add_argument('output',type=Path)
args=parser.parse_args();args.output.mkdir(exist_ok=False)

def audio_stats(samples):
    x=np.asarray(samples,dtype=np.float64)
    rms=float(np.sqrt(np.mean(x*x))) if x.size else 0
    peak=float(np.max(np.abs(x))) if x.size else 0
    return {'samples':int(x.size),'peak':peak,'rms':rms,
            'rms_dbfs':20*math.log10(rms) if rms else None,
            'nonzero_fraction':float(np.mean(x!=0)) if x.size else 0,
            'clipped_fraction':float(np.mean(np.abs(x)>=0.9999)) if x.size else 0}

report={'files':{},'decoder':av.__version__};thumbnails=[]
for path in sorted(args.media.rglob('*')):
    if not path.is_file() or path.suffix.lower() not in ['.wav','.mp4','.jpg','.jpeg']:continue
    rel=str(path.relative_to(args.media));entry={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()};report['files'][rel]=entry
    if path.suffix.lower() in ['.jpg','.jpeg']:
        with Image.open(path) as im:
            exif=im.getexif();details=exif.get_ifd(34665)
            entry.update(size=list(im.size),orientation=exif.get(274),exif={str(k):str(v) for k,v in details.items() if k in [33434,33437,34855,36867,37386]})
            image=ImageOps.exif_transpose(im).convert('RGB');x=np.asarray(image,dtype=np.float64);luma=x@np.array([.299,.587,.114])
            entry['rgb_percentiles']=[np.percentile(x[:,:,i],[0,1,5,50,95,99,100]).tolist() for i in range(3)]
            entry['luma_percentiles']=np.percentile(luma,[0,1,5,50,95,99,100]).tolist()
            entry['white_rgb_fraction']=float(np.mean(np.all(x>=250,axis=2)))
            entry['black_rgb_fraction']=float(np.mean(np.all(x<=5,axis=2)))
            thumb=image.copy();thumb.thumbnail((420,315));thumbnails.append((path.name,thumb))
        continue
    with av.open(str(path)) as container:
        entry['streams']=[]
        for stream in container.streams:
            ctx=stream.codec_context
            entry['streams'].append({'type':stream.type,'codec':ctx.name,'duration_s':float(stream.duration*stream.time_base) if stream.duration is not None else None,'time_base':str(stream.time_base),'metadata':dict(stream.metadata),
              **({'width':ctx.width,'height':ctx.height,'average_rate':str(stream.average_rate),'color_range':int(ctx.color_range),'colorspace':int(ctx.colorspace)} if stream.type=='video' else {'sample_rate':ctx.sample_rate,'channels':ctx.channels})})
    with av.open(str(path)) as container:
        if container.streams.audio:
            stream=container.streams.audio[0];resampler=av.AudioResampler(format='fltp',layout='mono',rate=48000);chunks=[];times=[]
            for frame in container.decode(stream):
                if frame.time is not None:times.append(frame.time)
                chunks.extend(f.to_ndarray().reshape(-1) for f in resampler.resample(frame))
            chunks.extend(f.to_ndarray().reshape(-1) for f in resampler.resample(None))
            x=np.concatenate(chunks) if chunks else np.zeros(0);stats=audio_stats(x)
            stats['duration_s']=len(x)/48000;stats['frame_time_start_end']=[times[0],times[-1]] if times else None;stats['one_second_rms']=[float(np.sqrt(np.mean(x[i:i+48000].astype(np.float64)**2))) for i in range(0,len(x),48000)]
            entry['audio']=stats
            dest=args.output/(path.stem+'-audio.wav')
            with wave.open(str(dest),'wb') as out:
                out.setnchannels(1);out.setsampwidth(2);out.setframerate(48000);out.writeframes(np.rint(np.clip(x,-1,1)*32767).astype('<i2').tobytes())
            entry['audio_preview']=str(dest)
    if path.suffix.lower()=='.mp4':
        with av.open(str(path)) as container:
            times=[];hashes=[];frame_images=[]
            for i,frame in enumerate(container.decode(video=0)):
                if frame.time is not None:times.append(frame.time)
                gray=frame.to_ndarray(format='gray');hashes.append(hashlib.sha256(gray.tobytes()).hexdigest())
                if i in [0,15,45,90]:
                    image=frame.to_image();image.thumbnail((640,640));dest=args.output/(path.stem+f'-frame-{i}.jpg');image.save(dest);frame_images.append(str(dest))
            delta=np.diff(times)
            entry['video']={'decoded_frames':len(hashes),'unique_frames':len(set(hashes)),'duration_pts_s':times[-1]-times[0] if len(times)>1 else 0,
                'effective_fps':(len(times)-1)/(times[-1]-times[0]) if len(times)>1 else None,'interval_ms_percentiles':(np.percentile(delta,[0,50,90,95,99,100])*1000).tolist() if delta.size else [],
                'intervals_over_100ms':int(np.sum(delta>.1)),'pts_s':times,'frame_previews':frame_images}
if thumbnails:
    columns=3;rows=math.ceil(len(thumbnails)/columns);sheet=Image.new('RGB',(columns*440,rows*350),'#dddddd');draw=ImageDraw.Draw(sheet)
    for i,(label,thumb) in enumerate(thumbnails):
        x=i%columns*440;y=i//columns*350;sheet.paste(thumb,(x+(440-thumb.width)//2,y+25));draw.text((x+8,y+5),label,fill='black')
    sheet.save(args.output/'photo-contact-sheet.jpg')
(args.output/'media-analysis.json').write_text(json.dumps(report,indent=2)+'\n')
for name,x in report['files'].items():
    print(name,json.dumps({k:x[k] for k in ['audio','video','size','orientation'] if k in x},default=str)[:900])
print('MEDIA_ANALYSIS_COMPLETE',args.output)
