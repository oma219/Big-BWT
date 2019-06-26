
// struct shared via mt_parse
typedef struct {
  Args *arg;       // command line input 
  MTmaps *mtmaps;  // collection of hash->WordFreq maps  
  long start, end; // input
  long skipped, parsed, words;  // output
  FILE *tmp_parse_file, *last_file, *sa_file;  
  
  void update(uint64_t hash, string &w);

} mt_data;


void mt_data::update(uint64_t hash, string &w) {
  
  int i = hash % mtmaps->n;
  map<uint64_t,word_stats> *freq = &mtmaps->maps[i];
  pthread_mutex_t *m = &mtmaps->muts[i]; 
  xpthread_mutex_lock(m,__LINE__,__FILE__);
  // update frequency table for current hash
  if(freq->find(hash)==freq->end()) {
      (*freq)[hash].occ = 1; // new hash
      (*freq)[hash].str = w; 
  }
  else {
      word_stats *wfreq = &(*freq)[hash];  // pointer to the stats for w
      wfreq->occ += 1; // known hash
      if(wfreq->occ <=0) {
        cerr << "Emergency exit! Maximum # of occurence of dictionary word (";
        cerr<< MAX_WORD_OCC << ") exceeded\n";
        exit(1);
      }
      if(wfreq->str != w) {
        cerr << "Emergency exit! Hash collision for strings:\n";
        cerr << wfreq->str << "\n  vs\n" <<  w << endl;
        exit(1);
      }
  }
  xpthread_mutex_unlock(m,__LINE__,__FILE__);
}


// save current word in the freq map and update it leaving only the 
// last minsize chars which is the overlap with next word  
static void mt_save_update_word(string& w, unsigned int minsize, uint64_t &pos, mt_data *d)
{
  assert(pos==0 || w.size() > minsize);
  if(w.size() <= minsize) return;
  // get the hash value and write it to the temporary parse file
  uint64_t hash = kr_hash(w);
  if(fwrite(&hash,sizeof(hash),1,d->tmp_parse_file)!=1) die("parse write error");

  // update the frequency  word w via its hash
  d->update(hash,w);

  // output char w+1 from the end
  if(fputc(w[w.size()- minsize-1],d->last_file)==EOF) die("Error writing to .last file");
  // compute ending position +1 of current word and write it to sa file 
  // pos is the ending position+1 of the previous word and is updated here 
  if(pos==0) pos = w.size()-1; // -1 is for the initial $ of the first word
  else pos += w.size() - minsize; 
  if(d->sa_file) if(fwrite(&pos,IBYTES,1,d->sa_file)!=1) die("Error writing to sa info file");
  // keep only the overlapping part of the window
  w.erase(0,w.size() - minsize);
}




// function executed by each thread to parse a segment of input files
// the tmp_parse, last and (optional) sa information is stored 
// in a different file for each thread
static void *mt_parse(void *dx)
{
  // extract input data
  mt_data *d = (mt_data *) dx;
  Args *arg = d->arg;

  if(arg->verbose>1)
    printf("Scanning from %ld, size %ld\n",d->start,d->end-d->start);

  // open input file 
  ifstream f(arg->inputFileName);
  if(!f.is_open()) {
    perror(__func__);
    throw new std::runtime_error("Cannot open file " + arg->inputFileName);
  }

  // prepare for parsing 
  f.seekg(d->start);      // move to the beginning of assigned region
  KR_window krw(arg->w);
  int c; string word = "";
  d->skipped = d->parsed = d->words = 0;
  if(d->start==0) {
    word.append(1,Dollar);// no need to reach the next kr-window 
  }
  else {   // reach the next breaking window  
    while( (c = f.get()) != EOF ) {
      if(c<=Dollar) die("Invalid char found in input file. Exiting...");
      d->skipped++;
      if(d->start + d->skipped == d->end + arg->w) {f.close(); return NULL;} 
      word.append(1,c);
      uint64_t hash = krw.addchar(c);
      if(hash%arg->p==0 && d->skipped >= arg->w) break;
    }
    if(c==EOF) {f.close(); return NULL;} // reached EOF without finding a breaking point nothing to do   
    d->parsed = arg->w;   // the kr-window is part of the next word
    d->skipped -= arg->w; // ... so w less chars have been skipped
    word.erase(0,word.size() - arg->w);// keep only the last w chars 
  }
  // cout << "Skipped: " << d->skipped << endl;
  
  // there is some parsing to do
  uint64_t pos = d->start;             // ending position+1 in text of previous word
  if(pos>0) pos+= d->skipped+ arg->w;  // or 0 for the first word  
  assert(IBYTES<=sizeof(pos)); // IBYTES bytes of pos are written to the sa info file 
  while( (c = f.get()) != EOF ) {
    if(c<=Dollar) die("Invalid char found in input file. Exiting...");
    word.append(1,c);
    uint64_t hash = krw.addchar(c);
    d->parsed++;
    if(hash%arg->p==0 && d->parsed>arg->w) {
      // end of word, save it and write its full hash to the output file
      // pos is the ending position+1 of previous word and is updated in the next call
      mt_save_update_word(word,arg->w,pos,d);
      d->words++;
      if(d->start+d->skipped+d->parsed>=d->end+arg->w) {f.close(); return NULL;}
    }    
  }
  // end of file reached 
  // virtually add w null chars at the end of the file and add the last word in the dict
  word.append(arg->w,Dollar);
  mt_save_update_word(word,arg->w,pos,d);
  // close input file and return 
  f.close();
  return NULL;
}


// multithread prefix free parse of a file 
// mtmaps contain a set of dictionaries associating to each
// hash value a string and its number of occurrences
static uint64_t mt_process_file(Args& arg, MTmaps &mtmaps)
{
  // get input file size 
  ifstream f(arg.inputFileName, std::ifstream::ate);
  if(!f.is_open()) {
    perror(__func__);
    throw new std::runtime_error("Cannot open input file " +arg.inputFileName);
  }
  long size = f.tellg();
  f.close();   

  // prepare and execute threads 
  assert(arg.th>0);
  pthread_t t[arg.th];
  mt_data td[arg.th];
  for(int i=0;i<arg.th;i++) {
    td[i].arg = &arg;
    td[i].mtmaps = &mtmaps;
    td[i].start = i*(size/arg.th); // range start
    td[i].end = (i+1==arg.th) ? size : (i+1)*(size/arg.th); // range end
    assert(td[i].end<=size);
    // open the 1st pass parsing file 
    td[i].tmp_parse_file = open_aux_file_num(arg.inputFileName.c_str(),EXTPARS0,i,"wb");
    // open output file containing the char at position -(w+1) of each word
    td[i].last_file = open_aux_file_num(arg.inputFileName.c_str(),EXTLST,i,"wb");  
    // if requested open file containing the ending position+1 of each word
    td[i].sa_file = arg.SAinfo ?open_aux_file_num(arg.inputFileName.c_str(),EXTSAI,i,"wb") : NULL;
    xpthread_create(&t[i],NULL,&mt_parse,&td[i],__LINE__,__FILE__);
  }
  
  // wait for the threads to finish (in order) and close output files
  long tot_char=0;
  for(int i=0;i<arg.th;i++) {
    xpthread_join(t[i],NULL,__LINE__,__FILE__);
    if(arg.verbose) {
      cout << "s:" << td[i].start << "  e:" << td[i].end << "  pa:";
      cout << td[i].parsed << "  sk:" << td[i].skipped << "  wo:" << td[i].words << endl;
    }
    // close thread-specific output files     
    fclose(td[i].tmp_parse_file);
    fclose(td[i].last_file);
    if(td[i].sa_file) fclose(td[i].sa_file);
    if(td[i].words>0) {
      // extra check
      assert(td[i].parsed>arg.w);
      tot_char += td[i].parsed - (i!=0? arg.w: 0); //parsed - overlapping 
    }
    else assert(i>0); // the first thread must produce some words
  }
  assert(tot_char==size);
  return size;   
}
