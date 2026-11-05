# Writes the browser build's index.html: app/web/index.html with the WebAssembly app and its shim loaded
# before the page script, plus the public look (app/web/public: fx.css, fx.js) that the app's own page leaves out.
# Run with -DPAGE=<index.html> -DOUT=<site/index.html>.
file(READ ${PAGE} page)
foreach(tag "<script>" "</head>" "</body>")
  string(FIND "${page}" "${tag}" at)
  if(at LESS 0)
    message(FATAL_ERROR "no ${tag} in ${PAGE}")
  endif()
endforeach()
string(REPLACE "</body>" "<script src=\"fx.js\"></script></body>" page "${page}")
string(FIND "${page}" "<script>" at)
set(inject "<script>if(!self.crossOriginIsolated&&navigator.serviceWorker)navigator.serviceWorker.register('coi-serviceworker.js').then(()=>{if(!navigator.serviceWorker.controller)location.reload()})</script>\n<script src=\"ucxapp.js\"></script><script src=\"ucxapp-wasm.js\"></script>\n")
string(SUBSTRING "${page}" 0 ${at} head)
string(SUBSTRING "${page}" ${at} -1 tail)
# FX off (the FX button) drops the whole look before the first paint; ?fx=1 or ?fx=0 in a link sets it
set(fx "<link id=\"fx-css\" rel=\"stylesheet\" href=\"fx.css\"><script>{const q=new URLSearchParams(location.search).get('fx');if(q==='1'||q==='0')localStorage.setItem('fxoff',q==='1'?'0':'1')}if(localStorage.getItem('fxoff')==='1')document.getElementById('fx-css').disabled=true</script></head>")
string(REPLACE "</head>" "${fx}" head "${head}")
string(REPLACE "</head>" "${fx}" tail "${tail}")
file(WRITE ${OUT} "${head}${inject}${tail}")
