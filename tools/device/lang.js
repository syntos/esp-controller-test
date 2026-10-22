var L={nl:{
title:'Fallback-regels',intro:'Deze regels laat de controller zelf uitvoeren wanneer Home Assistant niet bereikbaar is. Je beheert ze hier, zonder Home Assistant.',
st:'Status',normal:'Normaal',fb:'Fallback actief',ha:'Home Assistant',haOn:'verbonden',haOff:'niet bereikbaar',rules:'Regels',
stNote:'De controller schakelt over na 15 seconden zonder Home Assistant en gaat 10 seconden na terugkeer weer naar normaal.',
cur:'Regels op deze controller',none:'Er staan nog geen regels op deze controller.',when:'Wanneer',then:'Dan',rule:'Regel',input:'Ingang',output:'Uitgang',
tboth:'Schakelaar wijzigt',trising:'Knop ingedrukt',tfalling:'Knop losgelaten',
atoggle:'Omschakelen',aon:'Aanzetten',aoff:'Uitzetten',aon_for:'Aan gedurende',aoff_for:'Uit gedurende',
up:'Nieuwe regels laden',drop:'Sleep een bestand hierheen',or:'of klik om een .json-bestand te kiezen',send:'Naar controller sturen',
dl:'Huidige regels downloaden',clr:'Alles wissen',ed:'Open de editor',edNote:'Regels samenstellen? Gebruik de editor, sla het bestand op en laad het hier.',
cq:'Alle fallback-regels wissen?',ct:'De controller doet dan niets meer wanneer Home Assistant wegvalt, tot je nieuwe regels laadt.',cancel:'Annuleren',ok:'Wissen',
choose:'Kies eerst een bestand.',fail:'Mislukt: ',foot:'Deze pagina draait op de controller zelf.',
sTitle:'Kies een wachtwoord',sIntro:'Beveilig deze pagina met een wachtwoord. Gebruikersnaam: admin.',pw:'Wachtwoord (minstens 8 tekens)',save:'Opslaan',
sWarn:'Het wachtwoord gaat onversleuteld over je lokale netwerk. Hergebruik dus geen wachtwoord van elders.',short:'Minstens 8 tekens.'},
en:{
title:'Fallback rules',intro:'The controller runs these rules by itself when Home Assistant cannot be reached. You manage them here, without Home Assistant.',
st:'Status',normal:'Normal',fb:'Fallback active',ha:'Home Assistant',haOn:'connected',haOff:'away',rules:'Rules',
stNote:'The controller switches to fallback after 15 seconds without Home Assistant and returns to normal 10 seconds after it is back.',
cur:'Rules on this controller',none:'There are no rules on this controller yet.',when:'When',then:'Then',rule:'Rule',input:'Input',output:'Output',
tboth:'Switch changes',trising:'Button pressed',tfalling:'Button released',
atoggle:'Toggle',aon:'Switch on',aoff:'Switch off',aon_for:'On for',aoff_for:'Off for',
up:'Load new rules',drop:'Drop a file here',or:'or click to choose a .json file',send:'Send to controller',
dl:'Download current rules',clr:'Clear all',ed:'Open the editor',edNote:'Want to build rules? Use the editor, save the file and load it here.',
cq:'Clear all fallback rules?',ct:'The controller will then do nothing when Home Assistant is away, until you load new rules.',cancel:'Cancel',ok:'Clear',
choose:'Choose a file first.',fail:'Failed: ',foot:'This page runs on the controller itself.',
sTitle:'Choose a password',sIntro:'Protect this page with a password. User name: admin.',pw:'Password (at least 8 characters)',save:'Save',
sWarn:'The password travels unencrypted over your local network, so do not reuse a password from elsewhere.',short:'At least 8 characters.'}};
var lang='en';
try{lang=localStorage.getItem('lc.lang')||''}catch(e){}
if(lang!=='nl'&&lang!=='en')lang=(navigator.language||'').slice(0,2)==='nl'?'nl':'en';
function t(k){return L[lang][k]||L.en[k]||k}
function $(s){return document.querySelector(s)}
function el(tag,cls,txt){var e=document.createElement(tag);if(cls)e.className=cls;if(txt!=null)e.textContent=txt;return e}
function applyLang(){
document.documentElement.lang=lang;
document.querySelectorAll('[data-i]').forEach(function(n){n.textContent=t(n.getAttribute('data-i'))});
document.querySelectorAll('[data-p]').forEach(function(n){n.placeholder=t(n.getAttribute('data-p'))});
document.title=t(document.body.getAttribute('data-title'))+' – Logi.casa';
$('#lang').textContent=lang==='nl'?'EN':'NL';
if(window.onLang)window.onLang()}
function flip(){lang=lang==='nl'?'en':'nl';try{localStorage.setItem('lc.lang',lang)}catch(e){}applyLang()}
