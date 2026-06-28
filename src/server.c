/*  server.c ― Academia Course-Registration Portal (multi-threaded TCP server)
 *  CS-513  System Software  ▪  IIIT-B
 *
 *  Build:  gcc -Wall -Iinclude -pthread src/server.c src/utils.c -o server
 *
 *  Highlights
 *  ──────────
 *  ▸ crash-safe record editing (never modify a line in-place after strtok)
 *  ▸ no double-frees or leaks, no hidden NULs in files
 *  ▸ menus tolerate stray <Enter> presses; blank lines are skipped quietly
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <pthread.h>
 #include <netinet/in.h>
 #include <sys/socket.h>
 #include <sys/stat.h>
 #include <fcntl.h>
 #include <sys/types.h>
 
 #include "common.h"   /* PORT, STUDENT_FILE, FACULTY_FILE, COURSE_FILE … */
 #include "utils.h"    /* send_line(), recv_line(), lock_file()           */
 
 #define MAX_FIELD 128
 #define MAX_LINE  1024
 #define MAX_LIST  1024         /* long comma-separated lists              */
 
 /* ────────────────────── tiny mmap-ish helper ────────────────────── */
 
 typedef struct { int fd; size_t sz; char *buf; char **ln; int n; } File;
 
 /* open + (optionally) lock + load whole text file */
 static int load(const char *path, File *f, int lock_type)
 {
     f->fd = open(path, O_RDWR | O_CREAT, 0666);
     if (f->fd < 0) return -1;
     if (lock_file(f->fd, lock_type) < 0) return -1;
 
     struct stat st;  fstat(f->fd, &st);
     f->sz = (size_t)st.st_size;
 
     f->buf = malloc(f->sz + 1);
     if (f->sz) read(f->fd, f->buf, f->sz);
     f->buf[f->sz] = '\0';
 
     f->n  = 0;
     f->ln = NULL;
     if (f->sz) {
         f->ln = malloc(sizeof(char *) * (f->sz / 2 + 2));
         char *save, *tok = strtok_r(f->buf, "\n", &save);
         while (tok) {
             f->ln[f->n++] = tok;
             tok = strtok_r(NULL, "\n", &save);
         }
     }
     return 0;
 }
 
 static void save(File *f)                 /* write-back + unlock */
 {
     lseek(f->fd, 0, SEEK_SET);
     ftruncate(f->fd, 0);
     for (int i = 0; i < f->n; ++i) {
         write(f->fd, f->ln[i], strlen(f->ln[i]));
         write(f->fd, "\n", 1);
     }
     unlock_file(f->fd);
     close(f->fd);
     free(f->buf);
     free(f->ln);
 }
 
 static void release(File *f)              /* read-only close */
 {
     unlock_file(f->fd);
     close(f->fd);
     free(f->buf);
     free(f->ln);
 }
 
 /* ────────────────────── helper utilities ────────────────────── */
 
 /* Split a line into ≤4 fields without altering the original string */
 static int split_line(const char *src, char *fld[4])
 {
     static __thread char buf[MAX_LINE];
     strncpy(buf, src, sizeof buf);
     char *sav, *tok; int k = 0;
     tok = strtok_r(buf, "|", &sav);
     while (tok && k < 4) { fld[k++] = tok; tok = strtok_r(NULL, "|", &sav); }
     return k;
 }
 
 /* Read next non-blank line and return it as an int (-1 on EOF/error)     */
 static int recv_choice(int sock)
 {
     char buf[32];
     for (;;) {
         if (recv_line(sock, buf, sizeof buf) <= 0) return -1;
         buf[strcspn(buf, "\r\n")] = '\0';
         char *p = buf; while (*p == ' ' || *p == '\t') ++p;
         if (*p) return atoi(p);           /* ignore silent blank lines */
     }
 }
 
 /* skip blanks / comments in text files */
 static int is_skip_line(const char *s)
 {
     if (!*s) return 1;
     while (*s == ' ' || *s == '\t') ++s;
     return (*s == '\0' || *s == '#');
 }
 
 /* generic search — returns row index or –1 */
 static int find_row(File *f, const char *key, int key_field)
 {
     for (int i = 0; i < f->n; ++i) {
         if (is_skip_line(f->ln[i])) continue;
         char *fld[4];
         if (split_line(f->ln[i], fld) > key_field &&
             strcmp(fld[key_field], key) == 0)
             return i;
     }
     return -1;
 }
 
 /* ────────────────────── forward decls ────────────────────── */
 static void *client_thread(void *);
 
 /* admin */
 static void admin_menu(int);
 static void admin_add(int,const char*,const char*);
 static void admin_view(int,const char*,const char*);
 static void admin_toggle(int,int);
 static void admin_setpwd(int,const char*);
 
 /* faculty */
 static void faculty_menu(int,const char*);
 static void faculty_add_course(int,const char*);
 static void faculty_remove_course(int,const char*);
 static void faculty_view_enrollments(int,const char*);
 static void faculty_change_pwd(int,const char*);
 
 /* student */
 static void student_menu(int,const char*);
 static void student_enroll(int,const char*);
 static void student_unenroll(int,const char*);
 static void student_view(int,const char*);
 static void student_change_pwd(int,const char*);
 
 /* ─────────────────────────── main ─────────────────────────── */
 int main(void)
 {
     int ls = socket(AF_INET, SOCK_STREAM, 0);
     struct sockaddr_in sa = { .sin_family = AF_INET,
                               .sin_addr.s_addr = INADDR_ANY,
                               .sin_port = htons(PORT) };
     bind(ls, (void*)&sa, sizeof sa);
     listen(ls, 16);
     printf(">> Server listening on %d\n", PORT);
 
     for (;;) {
         int cs = accept(ls, NULL, NULL);
         int *p  = malloc(sizeof(int)); *p = cs;
         pthread_t t; pthread_create(&t, NULL, client_thread, p);
         pthread_detach(t);
     }
 }
 
 /* ────────────────────── authentication ────────────────────── */
 static int auth_admin(int s)
 {
     char u[64], p[64];
     send_line(s,"Admin username:\n"); if (recv_line(s,u,sizeof u)<=0) return 0;
     send_line(s,"Admin password:\n"); if (recv_line(s,p,sizeof p)<=0) return 0;
     u[strcspn(u,"\r\n")] = p[strcspn(p,"\r\n")] = '\0';
     return !strcmp(u,"admin") && !strcmp(p,"admin123");
 }
 
 static int auth_file(int s, const char *file, char *who)
 {
     char u[64], p[64];
     send_line(s,"Username:\n"); if (recv_line(s,u,sizeof u)<=0) return 0;
     send_line(s,"Password:\n"); if (recv_line(s,p,sizeof p)<=0) return 0;
     u[strcspn(u,"\r\n")] = p[strcspn(p,"\r\n")] = '\0';
 
     File f; if (load(file,&f,F_RDLCK)<0) return 0;
     int ok = 0;
     for (int i = 0; i < f.n; ++i) {
         if (is_skip_line(f.ln[i])) continue;
         char *fld[4]; int k = split_line(f.ln[i], fld);
         if (k >= 3 && !strcmp(fld[0],u) && !strcmp(fld[1],p) && fld[2][0]=='1'){
             ok = 1; strcpy(who,u); break;
         }
     }
     release(&f);
     return ok;
 }
 
 /* ────────────────────── per-client thread ────────────────────── */
 static void *client_thread(void *arg)
 {
     int s = *(int*)arg; free(arg);
 
     send_line(s,"................Welcome Back to Academia................\n"
                 "Login Type\n"
                 "Enter Your Choice { 1.Admin , 2.Professor , 3.Student }: \n");
 
     int role = recv_choice(s);
     if (role == -1) { close(s); return NULL; }
 
     if (role == 1) {
         if(!auth_admin(s)){ send_line(s,"Invalid credentials\n"); close(s); return NULL; }
         send_line(s,"[OK] Admin authenticated\n");
         admin_menu(s);
     }
     else if (role == 2) {
         char who[64]="";
         if(!auth_file(s,FACULTY_FILE,who)){ send_line(s,"Invalid\n"); close(s); return NULL; }
         send_line(s,"[OK] Faculty authenticated\n");
         faculty_menu(s,who);
     }
     else if (role == 3) {
         char who[64]="";
         if(!auth_file(s,STUDENT_FILE,who)){ send_line(s,"Invalid\n"); close(s); return NULL; }
         send_line(s,"[OK] Student authenticated\n");
         student_menu(s,who);
     }
     else send_line(s,"Bad choice\n");
 
     send_line(s,"Goodbye!\n");
     close(s);
     return NULL;
 }
 
 /*────────────────────────── ADMIN ──────────────────────────*/
 static void admin_menu(int s)
 {
     const char *menu =
         "\n........ Admin Menu ........\n"
         "1. Add Student      (username,password)\n"
         "2. View Student List\n"
         "3. Add Faculty      (username,password)\n"
         "4. View Faculty List\n"
         "5. Activate Student (username)\n"
         "6. Block Student    (username)\n"
         "7. Set Student Password (username,newPwd)\n"
         "8. Set Faculty Password (username,newPwd)\n"
         "9. Logout\nChoice:\n";
 
     for (;;) {
         send_line(s, menu);
         int c = recv_choice(s);
         if (c == -1) return;
         if      (c==1) admin_add(s,STUDENT_FILE,"Student");
         else if (c==2) admin_view(s,STUDENT_FILE,"Student");
         else if (c==3) admin_add(s,FACULTY_FILE,"Faculty");
         else if (c==4) admin_view(s,FACULTY_FILE,"Faculty");
         else if (c==5) admin_toggle(s,1);
         else if (c==6) admin_toggle(s,0);
         else if (c==7) admin_setpwd(s,STUDENT_FILE);
         else if (c==8) admin_setpwd(s,FACULTY_FILE);
         else if (c==9) return;
         else send_line(s,"Invalid choice\n");
     }
 }
 
 static void admin_add(int s, const char *file, const char *tag)
 {
     char u[64], p[64], prompt[64];
     snprintf(prompt,sizeof prompt,"New %s username:\n",tag);
     send_line(s,prompt);        if(recv_line(s,u,sizeof u)<=0)return;
     send_line(s,"Password:\n"); if(recv_line(s,p,sizeof p)<=0)return;
     u[strcspn(u,"\r\n")] = p[strcspn(p,"\r\n")] = '\0';
 
     File f; load(file,&f,F_WRLCK);
 
     int row = find_row(&f, u, 0);
     if (row != -1) {                         /* username exists */
         char *fld[4]; int k = split_line(f.ln[row], fld);
         if (k >= 3) {                        /* well-formed → refuse */
             release(&f);
             send_line(s,"User already exists\n");
             return;
         }
         /* malformed → we shall overwrite below */
     }
 
     /* append (or overwrite malformed entry) */
     if (row == -1) {                         /* append */
         dprintf(f.fd,"%s|%s|1|\n",u,p);
     } else {                                 /* overwrite */
         char newline[MAX_LINE];
         snprintf(newline,sizeof newline,"%s|%s|1|",u,p);
         f.ln[row] = strdup(newline);
         save(&f);
     }
     release(&f);
     send_line(s,"[OK] Added\n");
 }
 
 static void admin_view(int s,const char *file,const char *title)
 {
     File f; if(load(file,&f,F_RDLCK)<0){ send_line(s,"Error\n"); return; }
     char hdr[64]; snprintf(hdr,sizeof hdr,"\n%s List\n",title); send_line(s,hdr);
     for(int i=0;i<f.n;i++){
         if(is_skip_line(f.ln[i])) continue;
         char *fld[4]; int k = split_line(f.ln[i], fld);
         if(k<3) continue;
         char ln[128]; snprintf(ln,sizeof ln," - %-12s  [%s]\n",
                                fld[0], fld[2][0]=='1' ? "active" : "blocked");
         send_line(s,ln);
     }
     release(&f);
 }
 
 /* activate=1 → activate, 0 → block */
 static void admin_toggle(int s, int activate)
 {
     char u[64];
     send_line(s,"Student username:\n"); if(recv_line(s,u,sizeof u)<=0) return;
     u[strcspn(u,"\r\n")] = '\0';
 
     File f; load(STUDENT_FILE,&f,F_WRLCK);
     int row = find_row(&f,u,0);
     if(row<0){ release(&f); send_line(s,"User not found\n"); return; }
 
     char *fld[4]; int k = split_line(f.ln[row], fld);
     if(k < 3){ release(&f); send_line(s,"Malformed record\n"); return; }
 
     char newline[MAX_LINE];
     snprintf(newline,sizeof newline,"%s|%s|%c|%s",
              fld[0], fld[1], activate?'1':'0', (k==4?fld[3]:""));
     f.ln[row] = strdup(newline);
     save(&f);
     send_line(s,"[OK]\n");
 }
 
 static void admin_setpwd(int s,const char *file)
 {
     char u[64], p[64];
     send_line(s,"Username:\n");      if(recv_line(s,u,sizeof u)<=0) return;
     send_line(s,"New password:\n"); if(recv_line(s,p,sizeof p)<=0) return;
     u[strcspn(u,"\r\n")] = p[strcspn(p,"\r\n")] = '\0';
 
     File f; load(file,&f,F_WRLCK);
     int row = find_row(&f,u,0);
     if(row<0){ release(&f); send_line(s,"User not found\n"); return; }
 
     char *fld[4]; int k = split_line(f.ln[row], fld);
     char newline[MAX_LINE];
     snprintf(newline,sizeof newline,"%s|%s|%s|%s",
              fld[0], p, (k>=3?fld[2]:"1"), (k==4?fld[3]:""));
     f.ln[row] = strdup(newline);
     save(&f);
     send_line(s,"[OK]\n");
 }
 
 /*────────────────────────── FACULTY ──────────────────────────*/
 static void faculty_menu(int s,const char *who)
 {
     const char *menu =
         "\n........ Faculty Menu ........\n"
         "1. Add New Course      (courseID,courseName,seatLimit)\n"
         "2. Remove Course       (courseID)\n"
         "3. View Enrollments    (shows list per course)\n"
         "4. Change Password     (newPwd)\n"
         "5. Logout\nChoice:\n";
 
     for (;;) {
         send_line(s,menu);
         int c = recv_choice(s);
         if (c == -1) return;
         if      (c==1) faculty_add_course(s,who);
         else if (c==2) faculty_remove_course(s,who);
         else if (c==3) faculty_view_enrollments(s,who);
         else if (c==4) faculty_change_pwd(s,who);
         else if (c==5) return;
         else send_line(s,"Invalid choice\n");
     }
 }
 
 static void faculty_add_course(int s,const char *who)
 {
     char id[MAX_FIELD], name[MAX_FIELD], lim[16];
     send_line(s,"Course ID:\n");      if(recv_line(s,id,sizeof id)<=0)   return;
     send_line(s,"Course Name:\n");    if(recv_line(s,name,sizeof name)<=0)return;
     send_line(s,"Seat Limit:\n");     if(recv_line(s,lim,sizeof lim)<=0) return;
     id  [strcspn(id  ,"\r\n")]='\0';
     name[strcspn(name,"\r\n")]='\0';
     int limit = atoi(lim);
 
     /*---- catalogue ------------------------------------------------*/
     File fc; load(COURSE_FILE,&fc,F_WRLCK);
     int row = find_row(&fc,id,0);
     if(row<0){                                   /* new course */
         fc.ln = realloc(fc.ln,(fc.n+1)*sizeof(char*));
         char *line = malloc(strlen(id)+strlen(name)+32);
         sprintf(line,"%s|%s|%d|0",id,name,limit);
         fc.ln[fc.n++] = line;
     }
     save(&fc);
 
     /*---- add course to professor row ------------------------------*/
     File ff; load(FACULTY_FILE,&ff,F_WRLCK);
     int prow = find_row(&ff,who,0);
     if(prow>=0){
         char *fld[4]; int k = split_line(ff.ln[prow], fld);
         if(k>=3 && fld[2][0]=='1'){               /* active */
             char list[MAX_LIST]="";
             if(k==4 && strlen(fld[3]))
                 snprintf(list,sizeof list,"%s,%s",fld[3],id);
             else strcpy(list,id);
 
             char newline[MAX_LINE];
             snprintf(newline,sizeof newline,"%s|%s|%s|%s",
                      fld[0],fld[1],fld[2],list);
             ff.ln[prow] = strdup(newline);
         }
     }
     save(&ff);
     send_line(s,"[OK] Course added\n");
 }
 
 static void faculty_remove_course(int s,const char *who)
 {
     char cid[MAX_FIELD];
     send_line(s,"Course ID to remove:\n"); if(recv_line(s,cid,sizeof cid)<=0) return;
     cid[strcspn(cid,"\r\n")] = '\0';
 
     /*---- remove from catalogue ------------------------------------*/
     File fc; load(COURSE_FILE,&fc,F_WRLCK);
     int found = find_row(&fc,cid,0);
     if(found<0){ release(&fc); send_line(s,"Course not found\n"); return;}
 
     /* do NOT free the pointer (may belong to fc.buf) - just shift   */
     memmove(&fc.ln[found], &fc.ln[found+1], (fc.n-found-1)*sizeof(char*));
     fc.n--;
     save(&fc);
 
     /*---- remove from professor row --------------------------------*/
     File ff; load(FACULTY_FILE,&ff,F_WRLCK);
     int prow = find_row(&ff,who,0);
     if(prow>=0){
         char *fld[4]; int k = split_line(ff.ln[prow], fld);
         if(k==4 && fld[2][0]=='1'){
             char newlist[MAX_LIST]=""; int first=1;
             char *sub,*sv;
             sub = strtok_r(fld[3],",",&sv);
             while(sub){
                 if(strcmp(sub,cid)){
                     if(!first) strcat(newlist,",");
                     strcat(newlist,sub); first=0;
                 }
                 sub = strtok_r(NULL,",",&sv);
             }
             char newline[MAX_LINE];
             snprintf(newline,sizeof newline,"%s|%s|%s|%s",
                      fld[0],fld[1],fld[2],newlist);
             ff.ln[prow] = strdup(newline);
         }
     }
     save(&ff);
     send_line(s,"[OK] Course removed\n");
 }
 
 static void faculty_view_enrollments(int s,const char *who)
 {
     /* get professor's course list */
     char offered[MAX_LIST]="";
     File ff; load(FACULTY_FILE,&ff,F_RDLCK);
     int prow = find_row(&ff,who,0);
     if(prow>=0){
         char *fld[4]; int k = split_line(ff.ln[prow], fld);
         if(k==4 && fld[2][0]=='1') strcpy(offered,fld[3]);
     }
     release(&ff);
     if(!strlen(offered)){ send_line(s,"You offer no courses (or account blocked)\n"); return;}
 
     File fs; load(STUDENT_FILE,&fs,F_RDLCK);
 
     char *cid,*outer;
     cid = strtok_r(offered,",",&outer);
     while(cid){
         char hdr[64]; snprintf(hdr,sizeof hdr,"\n%s:\n",cid); send_line(s,hdr);
 
         for(int i=0;i<fs.n;i++){
             if(is_skip_line(fs.ln[i])) continue;
             char *fld[4]; int k = split_line(fs.ln[i], fld);
             if(k<4 || fld[2][0]!='1' || !strlen(fld[3])) continue;
 
             char *sub,*sv;
             sub = strtok_r(fld[3],",",&sv);
             while(sub){
                 if(!strcmp(sub,cid)){
                     char line[128]; snprintf(line,sizeof line," - %s\n",fld[0]);
                     send_line(s,line); break;
                 }
                 sub = strtok_r(NULL,",",&sv);
             }
         }
         cid = strtok_r(NULL,",",&outer);
     }
     release(&fs);
 }
 
 static void faculty_change_pwd(int s,const char *who)
 {
     char pw[MAX_FIELD];
     send_line(s,"New password:\n"); if(recv_line(s,pw,sizeof pw)<=0) return;
     pw[strcspn(pw,"\r\n")]='\0';
 
     File ff; load(FACULTY_FILE,&ff,F_WRLCK);
     int prow = find_row(&ff,who,0);
     if(prow>=0){
         char *fld[4]; int k = split_line(ff.ln[prow], fld);
         if(k>=3 && fld[2][0]=='1'){
             char newline[MAX_LINE];
             snprintf(newline,sizeof newline,"%s|%s|%s|%s",
                      fld[0],pw,fld[2],(k==4?fld[3]:""));
             ff.ln[prow] = strdup(newline);
             save(&ff);
             send_line(s,"[OK] Password changed\n");
             return;
         }
     }
     release(&ff);
     send_line(s,"Account is blocked – cannot change password\n");
 }
 
 /*────────────────────────── STUDENT ──────────────────────────*/
 static void student_menu(int s,const char *who)
 {
     const char *menu =
         "\n........ Student Menu ........\n"
         "1. Enroll in Course   (courseID)\n"
         "2. Drop Course        (courseID)\n"
         "3. View Enrolled Courses\n"
         "4. Change Password    (newPwd)\n"
         "5. Logout\nChoice:\n";
 
     for (;;) {
         send_line(s,menu);
         int c = recv_choice(s);
         if (c == -1) return;
         if      (c==1) student_enroll(s,who);
         else if (c==2) student_unenroll(s,who);
         else if (c==3) student_view(s,who);
         else if (c==4) student_change_pwd(s,who);
         else if (c==5) return;
         else send_line(s,"Invalid choice\n");
     }
 }
 
 static void student_enroll(int s,const char *user)
 {
     char cid[MAX_FIELD];
     send_line(s,"Course ID to enroll:\n"); if(recv_line(s,cid,sizeof cid)<=0) return;
     cid[strcspn(cid,"\r\n")]='\0';
 
     /* bump seats */
     File fc; load(COURSE_FILE,&fc,F_WRLCK);
     int row = find_row(&fc,cid,0);
     if(row<0){ release(&fc); send_line(s,"Course not found\n"); return; }
 
     char *fld[4]; int k = split_line(fc.ln[row], fld);
     int limit = atoi(fld[2]), filled = atoi(fld[3]);
     if(filled>=limit){ release(&fc); send_line(s,"Course full\n"); return; }
     filled++;
     char newline[MAX_LINE];
     snprintf(newline,sizeof newline,"%s|%s|%d|%d",fld[0],fld[1],limit,filled);
     fc.ln[row] = strdup(newline);
     save(&fc);
 
     /* add to student record */
     File fs; load(STUDENT_FILE,&fs,F_WRLCK);
     int srow = find_row(&fs,user,0);
     if(srow>=0){
         char *sfld[4]; k = split_line(fs.ln[srow], sfld);
         char list[MAX_LIST]="";
         if(k==4 && strlen(sfld[3])) snprintf(list,sizeof list,"%s,%s",sfld[3],cid);
         else strcpy(list,cid);
         snprintf(newline,sizeof newline,"%s|%s|%s|%s",
                  sfld[0],sfld[1],sfld[2],list);
         fs.ln[srow] = strdup(newline);
     }
     save(&fs);
     send_line(s,"[OK] Enrolled\n");
 }
 
 static void student_unenroll(int s,const char *user)
 {
     char cid[MAX_FIELD];
     send_line(s,"Course ID to drop:\n"); if(recv_line(s,cid,sizeof cid)<=0) return;
     cid[strcspn(cid,"\r\n")] = '\0';
 
     /* remove from student list */
     File fs; load(STUDENT_FILE,&fs,F_WRLCK);
     int srow = find_row(&fs,user,0); if(srow<0){ release(&fs); return; }
 
     char *sfld[4]; int k = split_line(fs.ln[srow], sfld);
     char newlist[MAX_LIST]=""; int first=1, had=0;
     if(k==4){
         char *sub,*sv; sub = strtok_r(sfld[3],",",&sv);
         while(sub){
             if(strcmp(sub,cid)){
                 if(!first) strcat(newlist,",");
                 strcat(newlist,sub); first=0;
             } else had=1;
             sub = strtok_r(NULL,",",&sv);
         }
     }
     if(!had){ release(&fs); send_line(s,"Not enrolled in that course\n"); return; }
 
     char newline[MAX_LINE];
     snprintf(newline,sizeof newline,"%s|%s|%s|%s",
              sfld[0],sfld[1],sfld[2],newlist);
     fs.ln[srow] = strdup(newline);
     save(&fs);
 
     /* decrement seats */
     File fc; load(COURSE_FILE,&fc,F_WRLCK);
     int row = find_row(&fc,cid,0);
     if(row>=0){
         char *fld[4]; k = split_line(fc.ln[row], fld);
         int filled = atoi(fld[3]); if(filled>0) filled--;
         snprintf(newline,sizeof newline,"%s|%s|%s|%d",fld[0],fld[1],fld[2],filled);
         fc.ln[row] = strdup(newline);
     }
     save(&fc);
     send_line(s,"[OK] Unenrolled\n");
 }
 
 static void student_view(int s,const char *user)
 {
     File fs; load(STUDENT_FILE,&fs,F_RDLCK);
     int srow = find_row(&fs,user,0); if(srow<0){ release(&fs); return; }
 
     char *fld[4]; int k = split_line(fs.ln[srow], fld);
     char list[MAX_LIST]=""; if(k==4) strncpy(list,fld[3],sizeof list);
     release(&fs);
 
     if(!strlen(list)){ send_line(s,"No courses enrolled\n"); return;}
 
     File fc; load(COURSE_FILE,&fc,F_RDLCK);
     send_line(s,"Enrolled:\n");
     char *cid,*sv; cid = strtok_r(list,",",&sv);
     while(cid){
         int row = find_row(&fc,cid,0);
         if(row>=0){
             char *fld2[2]; k = split_line(fc.ln[row], fld2);
             char line[MAX_LINE];
             snprintf(line,sizeof line," - %s : %s\n",fld2[0],fld2[1]);
             send_line(s,line);
         }
         cid = strtok_r(NULL,",",&sv);
     }
     release(&fc);
 }
 
 static void student_change_pwd(int s,const char *user)
 {
     char pw[MAX_FIELD];
     send_line(s,"New password:\n"); if(recv_line(s,pw,sizeof pw)<=0) return;
     pw[strcspn(pw,"\r\n")] = '\0';
 
     File fs; load(STUDENT_FILE,&fs,F_WRLCK);
     int row = find_row(&fs,user,0);
     if(row>=0){
         char *fld[4]; int k = split_line(fs.ln[row], fld);
         char newline[MAX_LINE];
         snprintf(newline,sizeof newline,"%s|%s|%s|%s",
                  fld[0],pw,fld[2],(k==4?fld[3]:""));
         fs.ln[row] = strdup(newline);
     }
     save(&fs);
     send_line(s,"[OK] Password changed\n");
 }
 