#!/usr/bin/env python3
"""V4 q8: distinguish stable guest backing/publication from scalar-load and
category-filter divergence in the exact selected secondary light records.
Not a source of intended game lighting, nor a whole-P8 proof."""
import argparse,json
from pathlib import Path
import numpy as np
from p8_paired_replay import Archive,need,digest,key
import v4_q8_contract as q

RECORD_WORDS=96
RECORD_COUNT=224
BYTES=RECORD_WORDS*RECORD_COUNT*4
STRIDE=48
FIELDS=(0,4,0xe0,0xe4,0xe8,0xf0,0xf4,0x10c)
BACKINGS=('light_backing_before','light_backing_after_copy','light_backing_after_dispatch')
DIRTY=('cpu_modified','gpu_modified','gpu_dirty','pending_publication','image_gpu_dirty')

def bound_identity(event,stage,files):
    need(len(event['buffers'])==len(stage['buffers'])==4,'four native bindings required')
    b=event['buffers'][2];src=stage['buffers'][2]
    need(b['slot']==src['slot']==2 and b['guest']==src['guest'] and b['native']==src['native']
        and b['binding_offset']==src['native_offset'] and not src['written']
        and b['bytes']==src['bytes']==BYTES,'light buffer identity')
    need(b['native_offset']>=b['binding_offset'] and b['native_offset']-b['binding_offset']+BYTES<=src['native_range'],'native light range')
    data=files[b['file']];need(len(data)==BYTES,'truncated native light records')
    d=list(stage['guest_operands']['buffer_descriptors'][2][:4])
    need(len(d)==4 and d[0]|((d[1]&0xffff)<<32)==b['guest'] and (d[1]>>16)&0x3fff==384
        and d[2]==RECORD_COUNT and d[3]==0x5204,'unsupported light descriptor')
    return b,data,d

def publication(event,meta,data,files):
    try:
        states=[]; payloads=[]; ticks=[]
        for name in BACKINGS:
            row=event[name]
            need(row['ok'] and row['guest']==meta['guest'] and row['bytes']==BYTES,'unavailable/mismatched backing '+name)
            buf=files[row['file']];need(len(buf)==BYTES,'truncated backing '+name)
            payloads.append(buf);ticks.extend((row['tick_before'],row['tick_after']))
            states.extend((row['state_before'],row['state_after']))
        need(ticks==sorted(ticks) and ticks[0]<=meta['tick']<=ticks[-1],'backing observation order')
        need(all(x==payloads[0] for x in payloads),'backing changed during observation')
        for state in states:
            need(state['valid'] and state['registered'] and not any(state[k] for k in DIRTY),'no stable CPU backing authority')
            need(state['bytes']==BYTES and state['native']==meta['native'] and
                state['address']>=state['buffer_guest'] and state['address']-state['buffer_guest']==meta['native_offset'] and
                state['buffer_size']>=meta['native_offset']+BYTES,'cache owner/offset mismatch')
        keys=('address','buffer_guest','buffer_size','native','unregister_epoch','gpu_generation')
        need(all(tuple(s[k] for k in keys)==tuple(states[0][k] for k in keys) for s in states),'ownership/generation changed')
        expected=np.frombuffer(payloads[0],'<u4').reshape(RECORD_COUNT,RECORD_WORDS)
        actual=np.frombuffer(data,'<u4').reshape(RECORD_COUNT,RECORD_WORDS)
        diff=np.argwhere(expected!=actual)
        first=[{'light':int(i),'offset':hex(4*int(j)),'guest':int(expected[i,j]),'native':int(actual[i,j])} for i,j in diff[:64]]
        return {'status':'DIVERGENCE' if len(diff) else 'MATCH','compared_words':expected.size,'different_words':len(diff),
            'first_differences':first,'different_lights':sorted({int(i) for i,j in diff}),
            'different_fields':[(int(i),int(j)*4) for i,j in diff]}
    except (ValueError,KeyError,IndexError,TypeError) as ex:
        return {'status':'INCOMPLETE','reason':str(ex)}

def compare_records(c,records,terms,lights,lane,stencil,desc,buf):
    need(len(set(lights))==len(lights) and all(0<=i<RECORD_COUNT for i in lights),'light index outside exact descriptor')
    need(bool(stencil&64),'selected material not admitted')
    seen=[i for i in range(len(records)) if records[i,0] or records[i,4] or records[i,6]]
    need(seen==sorted(lights),'missing/extra secondary record')
    category=1<<((stencil>>2)&15);result=[]
    for i in lights:
        r=records[i]; t=terms[1+q.TERMS+i]; b=buf[i]
        for off,pc in ((0,0xdf4),(4,0xdf4),(6,0xdf8)):
            q.marker(r,off,pc)
        need(list(map(int,r[34:38]))==desc,'consumed scalar descriptor mismatch')
        c.equal(0xdf4,'secondary_index',i,int(r[1]),lane)
        c.equal(0xdf4,'record_flag_load',int(b[0x5c//4]),int(r[2]),lane)
        c.equal(0x1f0,'material_category_bit',category,int(r[3]),lane)
        masked=category&int(b[0x5c//4])
        c.equal(0xdf4,'category_and',masked,int(r[5]),lane)
        c.equal(0xdf8,'category_exec',int(not masked),int(r[7]),lane)
        q.marker(t,0,0x1d5c);q.marker(t,10,0x1d64)
        rgb=[q.float32(t[j]) for j in (2,4,6)]
        info={'light':i,'record_flag':int(b[0x5c//4]),'category':category,'admitted':not masked,
            'contribution':rgb,'type':int(b[0]),'record_rgb':[q.float32(b[o//4]) for o in (0xe0,0xe4,0xe8)]}
        if masked:
            c.equal(0xdf8,'rejected_record_not_loaded',0,int(r[8]),lane)
            for j in (2,4,6):
                c.equal(0x1d5c,'rejected_light_zero',0,int(t[j])&0x7fffffff,lane)
        else:
            q.marker(r,8,0xe68)
            for j,off in enumerate(FIELDS):
                c.equal(0xe68,'dynamic_scalar_load_'+hex(off),int(b[off//4]),int(r[9+j]),lane)
            if r[17]:
                q.marker(r,17,0x194c);info['position']=[q.float32(x) for x in r[19:22]]
                info['pre_distance_gate']=q.float32(r[18])
            if r[22]:
                q.marker(r,22,0x1978);info['distance']=q.float32(r[23]);info['distance_squared']=q.float32(r[24])
                info['radius']=q.float32(b[0xf4//4])
            if r[25]:
                q.marker(r,25,0x19b4);info['attenuation']=q.float32(r[26])
            if r[28]:
                q.marker(r,28,0x1b0c);info['post_cutoff_factor']=q.float32(r[29])
            if r[38]:
                q.marker(r,38,0x1b18);info['rgb_before_cube']=[q.float32(x) for x in r[39:42]]
            if r[30]:
                q.marker(r,30,0x1bb4);info['rgb_after_cube']=[q.float32(x) for x in r[31:34]]
        result.append(info)
    return result

def audit(path,witness='dark'):
    a=Archive(path);report={'scope':'same-dispatch secondary light record publication and dynamic loads; not intended lighting or P8'}
    c=q.Checks()
    try:
        events=[(i,e) for i,e in enumerate(a.events) if e.get('kind')=='v4_q8_contract']
        need(len(events)==1,'exactly one V4 q8 event required')
        ei,e=events[0];ui,u=a.use[e['use']]
        need(e['schema'] in (2,3,4) and ui<ei and e['frame']==u['frame']==a.frame and e['submit']==u['submit'] and e['shader']==q.SHADER,'new record acquisition/dispatch identity required')
        st=u['stages'][0];need(st['valid'] and st['shader']==q.SHADER and digest(a.z.read('ds-lighting-capture/'+st['guest_file']))==q.HASH,'wrong guest shader')
        b,data,desc=bound_identity(e,st,a.files)
        pub=publication(e,b,data,a.files);report['publication']=pub
        e,trace=q.select_witness(e,a.files[e['trace_file']],witness)
        need(len(trace)==(q.WORDS+q.TERMS*STRIDE)*4,'missing extended trace')
        wg,lane=e['wg'],e['lane'];f,o,terms,samples=q.parse_trace(trace,wg,lane)
        need(0<=lane<64 and 0<=wg<e['groups'][0] and e['launch']['threads']==[8,8,1] and e['launch']['wave']==64,'launch mismatch')
        need(f[lane,17]==0x129 and f[lane,18]==1 and int(f[lane,16])==e['stencil'],'stencil/EXEC link')
        stencil=st['images'][0]
        ss=[s for s in stencil['snapshots'] if s.get('valid') and s['role']=='stencil' and s['layer']==0]
        need(len(ss)==1 and key(ss[0])==key(stencil['image']),'stencil snapshot identity')
        sx,sy=e['pixel'];sw,sh=ss[0]['extent'];need(0<=sx<sw and 0<=sy<sh,'pixel range')
        sb=a.snapshot(ss[0],'stencil',later=ui);need(sb[sy*sw+sx]==e['stencil'] and list(map(int,f[lane,12:14]))==[sx,sy],'actual stencil bytes')
        active=[i for i,r in enumerate(f) if r[17]==0x129 and r[18]==1]
        mb=e['buffers'][3];src=st['buffers'][3]
        need(mb['slot']==src['slot']==3 and mb['guest']==src['guest'] and mb['native']==src['native'] and mb['bytes']==src['bytes'] and mb['binding_offset']==src['native_offset'] and not src['written'],'mask buffer identity')
        maskdata=a.files[mb['file']];need(len(maskdata)==mb['bytes'],'mask buffer bytes')
        _,lights=q.verify_mask(c,f,o,active,np.frombuffer(maskdata,'<u4'),st['guest_operands'],mb)
        need(bool(lights),'no secondary light record in selected wave')
        records=np.frombuffer(trace,'<u4')[q.WORDS:].reshape(q.TERMS,STRIDE)
        report['witness']={'frame':e['frame'],'submit':e['submit'],'wg':wg,'lane':lane,'pixel':e['pixel'],'stencil':e['stencil']}
        report['lights']=compare_records(c,records,terms,lights,lane,e['stencil'],desc,np.frombuffer(data,'<u4').reshape(RECORD_COUNT,RECORD_WORDS))
        relevant={ (i,0x5c) for i in lights }
        for row in report['lights']:
            if row['admitted']:relevant.update((row['light'],off) for off in FIELDS)
        diff=pub.pop('different_fields',[])
        pub['different_consumed_fields']=[{'light':i,'offset':hex(off)} for i,off in diff if (i,off) in relevant]
        report['dynamic_loads_and_filter']='MATCH'
        report['checks']=c.counts
        if pub['status']=='MATCH':
            report['next']='record values already present in authoritative guest backing; follow their actual producer, not the HDR accumulator'
            report['status']='MATCH'
        elif pub['status']=='DIVERGENCE':
            report['status']='DIVERGENCE'
            report['next']='repair guest-to-native publication at a consumed field' if pub['different_consumed_fields'] else 'publication mismatch outside the measured field set; establish its effect before a lighting fix'
        else:
            report['status']='INCOMPLETE_PUBLICATION'
            report['next']='dynamic scalar loads and filtering are decided; backing authority not established'
        return report
    except q.Divergence as ex:
        report.update(status='DIVERGENCE',first=ex.row,checks=c.counts,next='first dynamic load or filter mismatch; correct that operation')
        return report
    except (ValueError,KeyError,IndexError,TypeError,OSError) as ex:
        report.update(status='INCOMPLETE',reason=str(ex),checks=c.counts)
        return report
    finally:a.close()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);a=p.parse_args()
    print(json.dumps(audit(a.archive),indent=2,allow_nan=False))
