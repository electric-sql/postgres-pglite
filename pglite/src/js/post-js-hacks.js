Module['setFS'] = function(newFS) {
  FS = newFS;
  Module['FS'] = newFS;
};