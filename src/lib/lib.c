/* Copyright (C) 2009, 2010, 2011  Olga Yakovleva <yakovleva.o.v@gmail.com> */

/* This program is free software: you can redistribute it and/or modify */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or */
/* (at your option) any later version. */

/* This program is distributed in the hope that it will be useful, */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the */
/* GNU General Public License for more details. */

/* You should have received a copy of the GNU General Public License */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistr.h>
#include <unicase.h>
#include <sox.h>
#include <math.h>
#include <HTS_engine.h>
#include "sonic.h"
#include "lib.h"
#include "vector.h"
#include "io_utils.h"
#include "mutex.h"
#include "settings.h"

static void synth_start(HTS_Engine *e,HTS_Vocoder *v)
{
  int i, j, k;
  int msd_frame;

  /* initialize */
  e->gss.nstream = HTS_PStreamSet_get_nstream(&e->pss);
  e->gss.total_frame = HTS_PStreamSet_get_total_frame(&e->pss);
  e->gss.total_nsample = e->global.fperiod * e->gss.total_frame;
  e->gss.gstream = (HTS_GStream *) calloc(e->gss.nstream, sizeof(HTS_GStream));
  for (i = 0; i < e->gss.nstream; i++) {
    e->gss.gstream[i].static_length = HTS_PStreamSet_get_static_length(&e->pss, i);
    e->gss.gstream[i].par =
      (double **) calloc(e->gss.total_frame, sizeof(double *));
    for (j = 0; j < e->gss.total_frame; j++)
      e->gss.gstream[i].par[j] =
        (double *) calloc(e->gss.gstream[i].static_length,
                          sizeof(double));
  }
  e->gss.gspeech = NULL;

  /* copy generated parameter */
  for (i = 0; i < e->gss.nstream; i++) {
    if (HTS_PStreamSet_is_msd(&e->pss, i)) {      /* for MSD */
      for (j = 0, msd_frame = 0; j < e->gss.total_frame; j++)
        if (HTS_PStreamSet_get_msd_flag(&e->pss, i, j)) {
          for (k = 0; k < e->gss.gstream[i].static_length; k++)
            e->gss.gstream[i].par[j][k] =
              HTS_PStreamSet_get_parameter(&e->pss, i, msd_frame, k);
          msd_frame++;
        } else
          for (k = 0; k < e->gss.gstream[i].static_length; k++)
            e->gss.gstream[i].par[j][k] = LZERO;
    } else {                  /* for non MSD */
      for (j = 0; j < e->gss.total_frame; j++)
        for (k = 0; k < e->gss.gstream[i].static_length; k++)
          e->gss.gstream[i].par[j][k] =
            HTS_PStreamSet_get_parameter(&e->pss, i, j, k);
    }
  }

  HTS_Vocoder_initialize(v, e->gss.gstream[0].static_length - 1, e->global.stage,
                         e->global.use_log_gain, e->global.sampling_rate, e->global.fperiod);
}

static void synth_frame(HTS_Engine *e,HTS_Vocoder *v,int i,short *osamples)
{
  int nlpf = 0;
  double *lpf = NULL;
  if (e->gss.nstream >= 3)
    {
      nlpf = (e->gss.gstream[2].static_length - 1) / 2;
      lpf = &e->gss.gstream[2].par[i][0];
    }
  HTS_Vocoder_synthesize(v, e->gss.gstream[0].static_length - 1,
                         e->gss.gstream[1].par[i][0],
                         &e->gss.gstream[0].par[i][0], nlpf, lpf,
                         e->global.alpha, e->global.beta,
                         e->global.volume, osamples, NULL);
}

static void synth_finish(HTS_Engine *e,HTS_Vocoder *v)
{
  HTS_Vocoder_clear(v);
}

cst_lexicon *cmu_lex_init();

static const char *lib_version=VERSION;

static int initialized=0;
cst_lexicon *en_lex=NULL;
static const sox_effect_handler_t *vol_effect_handler=NULL;
static const sox_effect_handler_t *hpf_effect_handler=NULL;

static RHVoice_callback user_callback=NULL;

vector_t(short,svector)
vector_t(RHVoice_event,eventlist)

typedef struct {
  HTS_Engine *engine;
  HTS_Vocoder vocoder;
  int next_frame;
  svector samples;
  svector osamples;
  sonicStream sstream;
  int sstream_flushed;
  float rate;
  float pitch;
  eventlist events;
  cst_item *cur_seg;
  int in_nsamples;
  int out_nsamples;
  int num_events;
  int event_index;
  int last_result;
  RHVoice_message message;
} synth_state;

static int synth_callback(const short *samples,int nsamples,synth_state *state);

typedef struct {
  synth_state *state;
} private_t;

static int getopts(sox_effect_t *e,int argc,char *argv[])
{
  private_t *p=(private_t*)(e->priv);
  p->state=(synth_state*)argv[1];
  return SOX_SUCCESS;
}

static int out_flow(sox_effect_t *e,const sox_sample_t *ib,sox_sample_t *ob,size_t *ins,size_t *ons)
{
  SOX_SAMPLE_LOCALS;
  synth_state *s=((private_t*)(e->priv))->state;
  *ons=0;
  int i;
  int res;
  short sample;
  int c=0;
  if(*ins>0)
    {
      if(!svector_reserve(s->osamples,svector_size(s->osamples)+*ins))
        return SOX_EOF;
      for(i=0;i<*ins;i++)
        {
          sample=SOX_SAMPLE_TO_SIGNED_16BIT(ib[i],c);
          svector_push(s->osamples,&sample);
        }
      if(svector_size(s->osamples)>=4800)
        {
          res=synth_callback(svector_data(s->osamples),svector_size(s->osamples),s);
          svector_clear(s->osamples);
          if(!res)
            return SOX_EOF;
        }
    }
  return SOX_SUCCESS;
}

static int out_drain(sox_effect_t *e,sox_sample_t *ob,size_t *ons)
{
  synth_state *s=((private_t*)(e->priv))->state;
  if(svector_size(s->osamples)>0)
    {
      synth_callback(svector_data(s->osamples),svector_size(s->osamples),s);
      svector_clear(s->osamples);
    }
  *ons=0;
  return SOX_EOF;
}

static int in_drain(sox_effect_t *e,sox_sample_t *ob,size_t *ons)
{
  SOX_SAMPLE_LOCALS;
  synth_state *s=((private_t *)(e->priv))->state;
  if(s->next_frame>=s->engine->gss.total_frame)
    {
      *ons=0;
      return SOX_EOF;
    }
  int fperiod=s->engine->global.fperiod;
  int i,j,n;
  n=*ons/fperiod;
  if(n>(s->engine->gss.total_frame-s->next_frame))
    n=s->engine->gss.total_frame-s->next_frame;
  if(n>10)
    n=10;
  short speech[80];
  for(i=0;i<n;i++)
    {
      synth_frame(s->engine,&s->vocoder,s->next_frame+i,speech);
      for(j=0;j<fperiod;j++)
        {
          ob[i*fperiod+j]=SOX_SIGNED_TO_SAMPLE(16,speech[j]);
        }
    }
  s->next_frame+=n;
  *ons=n*fperiod;
  return SOX_SUCCESS;
}

static int sonic_start(sox_effect_t *e)
{
  synth_state *s=((private_t*)(e->priv))->state;
  s->sstream=sonicCreateStream(s->engine->global.sampling_rate,1);
  if(s->sstream==NULL)
    return SOX_EOF;
  if(use_libsonic_for_pitch)
    sonicSetPitch(s->sstream,s->pitch);
  sonicSetSpeed(s->sstream,s->rate);
  sonicSetQuality(s->sstream,libsonic_hq?1:0);
  return SOX_SUCCESS;
}

static int sonic_stop(sox_effect_t *e)
{
  synth_state *s=((private_t*)(e->priv))->state;
  if(s->sstream)
    {
      sonicDestroyStream(s->sstream);
      s->sstream=NULL;
    }
  return SOX_SUCCESS;
}

static int sonic_flow(sox_effect_t *e,const sox_sample_t *ib,sox_sample_t *ob,size_t *ins,size_t *ons)
{
  SOX_SAMPLE_LOCALS;
  synth_state *s=((private_t*)(e->priv))->state;
  int i;
  short sample;
  int c=0;
  svector_clear(s->samples);
  if(*ins>0)
    {
      if(!svector_reserve(s->samples,*ins))
        {
          *ons=0;
          return SOX_EOF;
        }
      for(i=0;i<*ins;i++)
        {
          sample=SOX_SAMPLE_TO_SIGNED_16BIT(ib[i],c);
          svector_push(s->samples,&sample);
        }
      if(!sonicWriteShortToStream(s->sstream,svector_data(s->samples),svector_size(s->samples)))
        {
          *ons=0;
          return SOX_EOF;
        }
      svector_clear(s->samples);
    }
  if(*ons>sonicSamplesAvailable(s->sstream))
    *ons=sonicSamplesAvailable(s->sstream);
  if(*ons>0)
    {
      sample=0;
      if(!svector_resize(s->samples,*ons,&sample))
        {
          *ons=0;
          return SOX_EOF;
        }
      sonicReadShortFromStream(s->sstream,svector_data(s->samples),*ons);
      for(i=0;i<*ons;i++)
        {
          ob[i]=SOX_SIGNED_TO_SAMPLE(16,*svector_at(s->samples,i));
        }
    }
  return SOX_SUCCESS;
}

static int sonic_drain(sox_effect_t *e,sox_sample_t *ob,size_t *ons)
{
  SOX_SAMPLE_LOCALS;
  int i;
  short sample;
  synth_state *s=((private_t*)(e->priv))->state;
  if(!s->sstream_flushed)
    {
      sonicFlushStream(s->sstream);
      s->sstream_flushed=1;
    }
  if(sonicSamplesAvailable(s->sstream)==0)
    {
      *ons=0;
      return SOX_EOF;
    }
  if(*ons>sonicSamplesAvailable(s->sstream))
    *ons=sonicSamplesAvailable(s->sstream);
  if(*ons>0)
    {
      svector_clear(s->samples);
      sample=0;
      if(!svector_resize(s->samples,*ons,&sample))
        {
          *ons=0;
          return SOX_EOF;
        }
      sonicReadShortFromStream(s->sstream,svector_data(s->samples),*ons);
      for(i=0;i<*ons;i++)
        {
          ob[i]=SOX_SIGNED_TO_SAMPLE(16,*svector_at(s->samples,i));
        }
    }
  return SOX_SUCCESS;
}

static const sox_effect_handler_t in_effect_handler={"in",NULL,SOX_EFF_MCHAN,getopts,NULL,NULL,in_drain,NULL,NULL,sizeof(private_t)};
static const sox_effect_handler_t out_effect_handler={"out",NULL,SOX_EFF_MCHAN,getopts,NULL,out_flow,out_drain,NULL,NULL,sizeof(private_t)};
static const sox_effect_handler_t sonic_effect_handler={"sonic",NULL,SOX_EFF_MCHAN|SOX_EFF_LENGTH,getopts,sonic_start,sonic_flow,sonic_drain,sonic_stop,NULL,sizeof(private_t)};

#define synth_error(s) {s->last_result=0;return 0;}

static int synth_callback(const short *samples,int nsamples,synth_state *state)
{
  int i,d;
  RHVoice_event *events=NULL;
  int num_events=0;
  state->in_nsamples+=nsamples;
  for(i=state->event_index;i<state->num_events;i++)
    {
      d=eventlist_at(state->events,i)->audio_position-state->out_nsamples;
      if(d>=nsamples) break;
      num_events++;
      eventlist_at(state->events,i)->audio_position=d;
    }
  if(num_events>0) events=eventlist_at(state->events,state->event_index);
  state->last_result=user_callback(samples,nsamples,events,num_events,state->message);
  state->out_nsamples+=nsamples;
  state->event_index+=num_events;
  return state->last_result;
}

#define num_hts_data_files 15

static int open_hts_data_files(const char *path,FILE *file_list[num_hts_data_files]);
static void close_hts_data_files(FILE *file_list[num_hts_data_files]);

typedef struct
{
  const char *name;
  const char *open_mode;
  int is_optional;
} hts_data_file_info;

static const hts_data_file_info hts_data_list[num_hts_data_files]={
  {"dur.pdf","rb",0},
  {"tree-dur.inf","r",0},
  {"mgc.pdf","rb",0},
  {"tree-mgc.inf","r",0},
  {"mgc.win1","r",0},
  {"mgc.win2","r",0},
  {"mgc.win3","r",0},
  {"lf0.pdf","rb",0},
  {"tree-lf0.inf","r",0},
  {"lf0.win1","r",0},
  {"lf0.win2","r",0},
  {"lf0.win3","r",0},
  {"lpf.pdf","rb",0},
  {"tree-lpf.inf","r",0},
  {"lpf.win1","r",0}};

static int open_hts_data_files(const char *path,FILE *file_list[num_hts_data_files])
{
  int result=1;
  int i,l;
#ifdef WIN32
  char sep='\\';
#else
  char sep='/';
#endif
  char *full_path;
  char *name;
  memset(file_list,0,num_hts_data_files*sizeof(FILE*));
  l=strlen(path);
  if(l==0)
    return 0;
  full_path=calloc(l+20,sizeof(char));
  if(full_path==NULL)
    return 0;
  memcpy(full_path,path,l);
  if(full_path[l-1]!=sep)
    {
      full_path[l]=sep;
      name=full_path+l+1;
    }
  else
    name=full_path+l;
  for(i=0;i<num_hts_data_files;i++)
    {
      strcpy(name,hts_data_list[i].name);
      file_list[i]=my_fopen(full_path,hts_data_list[i].open_mode);
      if((file_list[i]==NULL)&&(!hts_data_list[i].is_optional))
        {
          result=0;
          break;
        }
    }
  free(full_path);
  if(!result)
    close_hts_data_files(file_list);
  return result;
}

static void close_hts_data_files(FILE *file_list[num_hts_data_files])
{
  int i;
  for(i=0;i<num_hts_data_files;i++)
    {
      if(file_list[i]!=NULL)
        {
          fclose(file_list[i]);
          file_list[i]=NULL;
        }
    }
}

typedef struct
{
  int id;
  HTS_Engine *engine;
  int is_free;
} engine_resource;

void engine_resource_free(engine_resource *r)
{
  HTS_Engine_clear(r->engine);
  free(r->engine);
}

typedef struct
{
  uint8_t *name;
  uint8_t *path;
} voice;

void voice_free(voice *v)
{
  free(v->name);
  free(v->path);
}

vector_t(engine_resource,reslist)
vector_t(voice,voicelist)

static struct {
  MUTEX mutex;
  reslist engine_resources;
  voicelist voices;
} engine_pool;

static int add_voice(const char *voice_path,void *user_data)
{
  FILE *file_list[num_hts_data_files];
  if(!open_hts_data_files(voice_path,file_list)) return 1;
  close_hts_data_files(file_list);
  voice v;
  v.path=u8_strdup((const uint8_t*)voice_path);
  if(v.path==NULL) goto err0;
  #ifdef WIN32
  const uint8_t *p=u8_strrchr(v.path,'\\')+1;
#else
  const uint8_t *p=u8_strrchr(v.path,'/')+1;
#endif
  size_t n=0;
  v.name=u8_totitle(p,u8_strlen(p)+1,NULL,NULL,NULL,&n);
  if(v.name==NULL) goto err1;
  if(!voicelist_push(engine_pool.voices,&v)) goto err2;
  return 1;
  err2: free(v.name);
  err1: free(v.path);
  err0: return 1;
}

int RHVoice_get_voice_count()
{
  return (engine_pool.voices!=NULL)?voicelist_size(engine_pool.voices):0;
}

static const voice *get_voice_by_id(int id)
{
  if((id>0)&&(id<=RHVoice_get_voice_count()))
    return voicelist_at(engine_pool.voices,id-1);
  else
    return NULL;
}

const char *RHVoice_get_voice_name(int id)
{
  if((id>0)&&(id<=RHVoice_get_voice_count()))
    return (const char*)(get_voice_by_id(id)->name);
  else
    return NULL;
}

int RHVoice_find_voice(const char *name)
{
  int n=RHVoice_get_voice_count();
  int i;
  for(i=1;i<=n;i++)
    {
      if(u8_strcmp((const uint8_t*)name,get_voice_by_id(i)->name)==0)
        return i;
    }
  return 0;
}

int initialize_engine(HTS_Engine *engine,const char *path)
{
  int i;
  FILE *fp[num_hts_data_files];
  HTS_Engine_initialize(engine,3);
  if(!open_hts_data_files(path,fp)) return 0;
  i=0;
  HTS_Engine_load_duration_from_fp(engine,&fp[i],&fp[i+1],1);
  i+=2;
  HTS_Engine_load_parameter_from_fp(engine,&fp[i],&fp[i+1],&fp[i+2],0,FALSE,3,1);
  i+=5;
  HTS_Engine_load_parameter_from_fp(engine,&fp[i],&fp[i+1],&fp[i+2],1,TRUE,3,1);
  i+=5;
  HTS_Engine_load_parameter_from_fp(engine,&fp[i],&fp[i+1],&fp[i+2],2,FALSE,1,1);
  i+=3;
  close_hts_data_files(fp);
  HTS_Engine_set_sampling_rate(engine,16000);
  HTS_Engine_set_fperiod(engine,80);
  HTS_Engine_set_alpha(engine,0.42);
  HTS_Engine_set_gamma(engine,0);
  HTS_Engine_set_log_gain(engine,TRUE);
  HTS_Engine_set_beta(engine,0.4);
  HTS_Engine_set_msd_threshold(engine,1,0.5);
  HTS_Engine_set_duration_interpolation_weight(engine,0,1.0);
  HTS_Engine_set_parameter_interpolation_weight(engine,0,0,1.0);
  HTS_Engine_set_parameter_interpolation_weight(engine,1,0,1.0);
  HTS_Engine_set_parameter_interpolation_weight(engine,2,0,1.0);
  return engine->global.sampling_rate;
}

int RHVoice_initialize(const char *data_path,RHVoice_callback callback,const char *cfg_path)
{
  if((data_path==NULL)||(callback==NULL)) return 0;
  user_callback=callback;
  if(en_lex==NULL) en_lex=cmu_lex_init();
  engine_pool.voices=voicelist_alloc(0,voice_free);
  if(engine_pool.voices==NULL) goto err1;
  for_each_dir_in_dir(data_path,add_voice,NULL);
  if(voicelist_size(engine_pool.voices)==0) goto err2;
  engine_pool.engine_resources=reslist_alloc(0,engine_resource_free);
  if(engine_pool.engine_resources==NULL) goto err2;
  if(sox_init()!=SOX_SUCCESS) goto err3;
  vol_effect_handler=sox_find_effect("vol");
  hpf_effect_handler=sox_find_effect("highpass");
  INIT_MUTEX(&engine_pool.mutex);
  load_settings(cfg_path);
  initialized=1;
  return 16000;
  err3: reslist_free(engine_pool.engine_resources);
  engine_pool.engine_resources=NULL;
  err2: voicelist_free(engine_pool.voices);
  engine_pool.voices=NULL;
  err1: return 0;
}

void RHVoice_terminate()
{
  if(!initialized) return;
  free_settings();
  DESTROY_MUTEX(&engine_pool.mutex);
  reslist_free(engine_pool.engine_resources);
  engine_pool.engine_resources=NULL;
  voicelist_free(engine_pool.voices);
  engine_pool.voices=NULL;
  sox_quit();
  vol_effect_handler=NULL;
  hpf_effect_handler=NULL;
  initialized=0;
}

HTS_Engine *get_engine(int id)
{
  const voice *v=get_voice_by_id(id);
  if(v==NULL) return NULL;
  size_t n,i;
  engine_resource r,*pr;
  HTS_Engine *p=NULL;
  LOCK_MUTEX(&engine_pool.mutex);
  n=reslist_size(engine_pool.engine_resources);
  for(i=0;i<n;i++)
    {
      pr=reslist_at(engine_pool.engine_resources,i);
      if((pr->id==id)&&pr->is_free)
        {
          p=pr->engine;
          pr->is_free=0;
          break;
        }
    }
  UNLOCK_MUTEX(&engine_pool.mutex);
  if(p==NULL)
    {
      r.engine=calloc(1,sizeof(HTS_Engine));
      if(r.engine==NULL) return NULL;
      if(!initialize_engine(r.engine,(const char*)v->path))
        {
          free(r.engine);
          return NULL;
        }
      r.is_free=0;
      r.id=id;
      LOCK_MUTEX(&engine_pool.mutex);
      if(reslist_push(engine_pool.engine_resources,&r))
        p=(reslist_back(engine_pool.engine_resources)->engine);
      UNLOCK_MUTEX(&engine_pool.mutex);
      if(p==NULL)
        {
          HTS_Engine_clear(r.engine);
          free(r.engine);
        }
    }
  return p;
}

void release_engine(HTS_Engine *engine)
{
  size_t n,i;
  engine_resource *pr;
  LOCK_MUTEX(&engine_pool.mutex);
  n=reslist_size(engine_pool.engine_resources);
  for(i=0;i<n;i++)
    {
      pr=reslist_at(engine_pool.engine_resources,i);
      if(engine==pr->engine)
        pr->is_free=1;
    }
  UNLOCK_MUTEX(&engine_pool.mutex);
}

const char *RHVoice_get_version()
{
  return lib_version;
}

static eventlist generate_events(cst_utterance *u)
{
  eventlist events=eventlist_alloc(0,NULL);
  RHVoice_event e;
  cst_item *t,*s;
  e.audio_position=0;
  s=item_daughter(relation_head(utt_relation(u,"Transcription")));
  if(s) e.audio_position=item_feat_int(s,"expected_start");
  e.message=(RHVoice_message)val_userdata(feat_val(u->features,"message"));
  for(t=relation_head(utt_relation(u,"Token"));t;t=item_next(t))
    {
      s=path_to_item(t,"daughter1.R:Transcription.daughter1");
      if(s) e.audio_position=item_feat_int(s,"expected_start");
      if(item_feat_present(t,"mark_name"))
        {
          e.type=RHVoice_event_mark;
          e.text_position=item_feat_int(t,"mark_position");
          e.text_length=0;
          e.id.name=item_feat_string(t,"mark_name");
          eventlist_push(events,&e);
        }
      e.text_position=item_feat_int(t,"position");
      if(!item_prev(t))
        {
          e.type=RHVoice_event_sentence_start;
          e.text_length=feat_int(u->features,"length");
          e.id.number=feat_int(u->features,"number");
          eventlist_push(events,&e);
        }
      e.type=RHVoice_event_word_start;
      e.text_length=item_feat_int(t,"length");
      e.id.number=item_feat_int(t,"number");
      eventlist_push(events,&e);
      if(s)
        {
          s=path_to_item(t,"daughtern.R:Transcription.daughtern");
          e.audio_position=item_feat_int(s,"expected_end");
        }
      e.text_position+=e.text_length;
      e.text_length=0;
      e.type=RHVoice_event_word_end;
      eventlist_push(events,&e);
      if(!item_next(t))
        {
          e.type=RHVoice_event_sentence_end;
          e.id.number=feat_int(u->features,"number");
          eventlist_push(events,&e);
        }
    }
  return events;
}

static const char* sonorants[]={"m","mm","n","nn","l","ll","r","rr","j"};

static void fix_f0(cst_utterance *u,HTS_Engine *e)
{
  cst_item *t=relation_tail(utt_relation(u,"Token"));
  if(t==NULL) return;
  const char *p=item_feat_string(t,"punc");
  if(p[0]=='\0')
    return;
  const char *q=strchr(p,'?');
  if((!q)&&(strchr(p,'.')||strchr(p,'!')||strchr(p,':')||strchr(p,';')))
    return;
  cst_item *w;
  for(w=item_last_daughter(relation_tail(utt_relation(u,"Phrase")));w;w=item_prev(w))
    {
      if(cst_streq(ffeature_string(w,"gpos"),"content")) break;
    }
  if(w==NULL) return;
  cst_item *syl;
  for(syl=path_to_item(w,"R:SylStructure.daughtern");syl;syl=item_prev(syl))
    {
      if(cst_streq(item_feat_string(syl,"stress"),"1")) break;
    }
  if(syl==NULL) return;
  cst_item *v=path_to_item(syl,"R:SylVowel.daughter1.R:Segment");
  if(v==NULL) return;
  cst_item *pv=path_to_item(v,"R:SylVowel.parent.p.daughter1.R:Segment");
  int n;
  cst_item *s;
  for(n=0,s=relation_head(utt_relation(u,"Segment"));s!=v;s=item_next(s),n++);
  int ns=e->sss.nstate;
  int start=ns*n;
  int end=start+ns-1;
  double f0;
  double B=HTS_SStreamSet_get_mean(&e->sss,1,start,0);
  int i,j;
  for(i=start+1;i<e->sss.total_state;i++)
    {
      f0=HTS_SStreamSet_get_mean(&e->sss,1,i,0);
      if((f0>0)&&(f0<B))
        B=f0;
    }
  double T=B+log(2);
  double M=B+0.5*log(2);
  for(s=path_to_item(v,"R:Segment.p"),i=(n-1)*ns;s;s=item_prev(s),i-=ns)
    {
      if(s==pv)
        {
          M=0;
          for(j=0;j<ns;j++)
            {
              M+=HTS_SStreamSet_get_mean(&e->sss,1,i+j,0)/ns;
            }
          T=M+log(2);
          B=M-log(2);
          break;
        }
    }
  if(cst_member_string(ffeature_string(v,"R:SylStructure.p.name"),sonorants))
    {
      f0=M;
      for(i=start-ns;i<start;i++)
        {
          f0=0.95*f0+0.05*T;
          HTS_SStreamSet_set_mean(&e->sss,1,i,0,f0);
        }
    }
  else
    f0=M;
  if(q)
  f0=0.5*f0+0.5*T;
  else
    f0=0.75*f0+0.25*T;
  HTS_SStreamSet_set_mean(&e->sss,1,start,0,f0);
  f0=0.95*f0+0.05*B;
  HTS_SStreamSet_set_mean(&e->sss,1,start+1,0,f0);
  f0=0.95*f0+0.05*B;
  HTS_SStreamSet_set_mean(&e->sss,1,start+2,0,f0);
  f0=0.95*f0+0.05*B;
  HTS_SStreamSet_set_mean(&e->sss,1,start+3,0,f0);
  f0=0.5*f0+0.5*B;
  HTS_SStreamSet_set_mean(&e->sss,1,start+4,0,f0);
}

cst_utterance *hts_synth(cst_utterance *u)
{
  sox_effect_t *e;
  sox_effects_chain_t *c;
  char *opts[1];
  HTS_LabelString *lstring=NULL;
  double *dur_mean,*dur_vari,local_dur;
  char **labels=NULL;
  int i,j,nlabels;
  double nframes,total_nframes;
  cst_item *s,*t;
  int use_libsonic=0;
  float pitch,rate,volume,f0;
  char strvolume[8];
  char *volopts[3]={strvolume,"amplitude","0.02"};
  char * hpfopts[1]={"350"};
  HTS_Engine *engine=get_engine(get_param_int(u->features,"voice_id",1));
  if(engine==NULL) return NULL;
  synth_state state;
  sox_encodinginfo_t enc={SOX_ENCODING_SIGN2,16,0,SOX_OPTION_DEFAULT,SOX_OPTION_DEFAULT,SOX_OPTION_DEFAULT,sox_false};
  sox_signalinfo_t isig={engine->global.sampling_rate,1,16,SOX_UNSPEC,NULL};
  sox_signalinfo_t osig={engine->global.sampling_rate,1,16,SOX_UNSPEC,NULL};
  state.engine=engine;
  for(nlabels=0,s=relation_head(utt_relation(u,"Segment"));s;nlabels++,s=item_next(s)) {}
  if(nlabels==0)
    {
      release_engine(engine);
      return NULL;
    }
  state.samples=svector_alloc(9600,NULL);
  if(state.samples==NULL)
    {
      release_engine(engine);
      return NULL;
    }
  state.osamples=svector_alloc(9600,NULL);
  if(state.osamples==NULL)
    {
      svector_free(state.samples);
      release_engine(engine);
      return NULL;
    }
  rate=feat_float(u->features,"rate");
  pitch=feat_float(u->features,"pitch");
  use_libsonic=(((rate!=1.0)&&(rate>=min_sonic_rate))||((pitch!=1.0)&&(use_libsonic_for_pitch)));
  volume=feat_float(u->features,"volume");
  float frames_per_sec=(float)engine->global.sampling_rate/((float)engine->global.fperiod);
  float factor,time;
  create_hts_labels(u);
  labels=(char**)calloc(nlabels,sizeof(char*));
  for(i=0,s=relation_head(utt_relation(u,"Segment"));s;s=item_next(s),i++)
    {
      labels[i]=(char*)item_feat_string(s,"hts_label");
    }
  HTS_Engine_load_label_from_string_list(engine,labels,nlabels);
  free(labels);
  dur_mean=(double*)calloc(engine->ms.nstate,sizeof(double));
  dur_vari=(double*)calloc(engine->ms.nstate,sizeof(double));
  engine->label.head->start=0;
  for(s=relation_head(utt_relation(u,"Segment")),lstring=engine->label.head;s;s=item_next(s),lstring=lstring->next)
    {
      HTS_ModelSet_get_duration(&engine->ms,lstring->name,dur_mean,dur_vari,engine->global.duration_iw);
      local_dur=0;
      for(i=0;i<engine->ms.nstate;i++)
        {
          local_dur+=dur_mean[i];
        }
      local_dur/=(use_libsonic?1.0:rate);
      if(cst_streq(item_name(s),"pau"))
        {
          factor=item_feat_float(s,"factor");
          time=item_feat_float(s,"time");
          local_dur*=factor;
          local_dur+=(use_libsonic?1.0:(1.0/rate))*time*frames_per_sec;
        }
      lstring->end=lstring->start+local_dur;
      if(lstring->next)
        lstring->next->start=lstring->end;
    }
  free(dur_vari);
  free(dur_mean);
  HTS_Label_set_frame_specified_flag(&engine->label,TRUE);
  HTS_Engine_create_sstream(engine);
  fix_f0(u,engine);
  total_nframes=0;
  for(s=relation_head(utt_relation(u,"Segment")),i=0;s;s=item_next(s),i++)
    {
      if(!use_libsonic_for_pitch)
        {
          t=path_to_item(s,"R:Transcription.parent.R:Token.parent");
          if(t)
            pitch=item_feat_float(t,"pitch");
          for(j=0;j<engine->sss.nstate;j++)
            {
              f0=exp(HTS_SStreamSet_get_mean(&engine->sss,1,i*engine->sss.nstate+j,0))*pitch;
              if(f0<10.0) f0=10.0;
              HTS_SStreamSet_set_mean(&engine->sss,1,i*engine->sss.nstate+j,0,log(f0));
            }
        }
      nframes=0;
      for(j=0;j<engine->sss.nstate;j++)
        {
          nframes+=HTS_SStreamSet_get_duration(&engine->sss,i*engine->sss.nstate+j);
        }
      item_set_int(s,"expected_start",(int)(((float)engine->global.fperiod)*total_nframes*(use_libsonic?(1.0/rate):1.0)));
      total_nframes+=nframes;
      item_set_int(s,"expected_end",(int)(((float)engine->global.fperiod)*total_nframes*(use_libsonic?(1.0/rate):1.0)));
    }
  HTS_Engine_create_pstream(engine);
  state.rate=rate;
  state.pitch=use_libsonic_for_pitch?pitch:1.0;
  state.sstream=NULL;
  state.sstream_flushed=0;
  state.events=generate_events(u);
  state.num_events=eventlist_size(state.events);
  state.event_index=0;
  state.cur_seg=relation_head(utt_relation(u,"Segment"));
  state.in_nsamples=0;
  state.out_nsamples=0;
  state.last_result=1;
  state.message=(RHVoice_message)val_userdata(feat_val(u->features,"message"));
  state.next_frame=0;
  synth_start(engine,&state.vocoder);
  c=sox_create_effects_chain(&enc,&enc);
  opts[0]=(char*)(&state);
  e=sox_create_effect(&in_effect_handler);
  sox_effect_options(e,1,opts);
  sox_add_effect(c,e,&isig,&osig);
  /* The way code examples included in the sox source distribution are written */
/* suggests  that the effect is now owned by the chain and will be freed when the chain is deleted. */
  /* But sox_add_effect does not store the effect pointer in the chain, */
  /* it copies the contents of the structure into the freshly allocated memory region. */
/*   So we have a memory leak, which is a problem for us, */
/* because we often create and destroy effects and effect chains. */
/* I do not know if we are supposed to free the structure ourselves, */
/* or this problem must be fixed by the developers of libsox. */
  /* Someone has already opened a corresponding bug, */
/* but the developers have not commented on it yet. */
  free(e);
  if(use_libsonic)
    {
      e=sox_create_effect(&sonic_effect_handler);
      sox_effect_options(e,1,opts);
      sox_add_effect(c,e,&isig,&osig);
      free(e);
    }
  if(apply_high_pass_filter)
    {
      e=sox_create_effect(hpf_effect_handler);
      sox_effect_options(e,1,hpfopts);
      sox_add_effect(c,e,&isig,&osig);
      free(e);
    }
  if(volume!=1)
    {
      snprintf(strvolume,sizeof(strvolume),"%.4f",volume);
      e=sox_create_effect(vol_effect_handler);
      sox_effect_options(e,3,volopts);
      sox_add_effect(c,e,&isig,&osig);
      free(e);
    }
  e=sox_create_effect(&out_effect_handler);
  sox_effect_options(e,1,opts);
  sox_add_effect(c,e,&isig,&osig);
  free(e);
  sox_flow_effects(c,NULL,NULL);
  sox_delete_effects_chain(c);
  synth_finish(engine,&state.vocoder);
  HTS_Engine_refresh(engine);
  release_engine(engine);
  feat_set_int(u->features,"last_user_callback_result",state.last_result);
  if(state.events!=NULL) eventlist_free(state.events);
  svector_free(state.samples);
  svector_free(state.osamples);
  return u;
}

int RHVoice_speak(RHVoice_message msg)
{
  int last_user_callback_result=1;
  if(msg==NULL) return 0;
  if(!initialized) return 0;
  cst_utterance *u;
  for(u=next_utt_from_message(msg);u;u=next_utt_from_message(msg))
    {
      russian_init(u);
      utt_synth_tokens(u);
      last_user_callback_result=get_param_int(u->features,"last_user_callback_result",1);
      delete_utterance(u);
      if(!last_user_callback_result) break;
    }
  if(last_user_callback_result)
    report_final_mark(msg,user_callback);
  return 1;
}


