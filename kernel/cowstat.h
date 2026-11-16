#ifndef XV6_COWSTAT_H
#define XV6_COWSTAT_H
struct cowstat {
  uint64 freepages;
  uint64 sharedpages;
  uint64 cowfaults;
  uint64 cowcopies;
};
#endif
