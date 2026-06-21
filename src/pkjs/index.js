// LOTSE NG — Companion JS (Open-Meteo + Nominatim reverse-geocode, no API key)
// Fetches current weather via phone geolocation and sends compact strings to the watch.

var KEY_TEMPERATURE       = 0;
var KEY_CONDITIONS        = 1;
var KEY_REQUEST_WEATHER   = 2;
var KEY_CITY              = 3;

var weatherTimer = null;

function sendToWatch(dict) {
  Pebble.sendAppMessage(dict,
    function()  { console.log('AppMessage sent'); },
    function(e) { console.log('AppMessage send failed: ' + JSON.stringify(e)); }
  );
}

function weatherCodeToShort(code) {
  var map = {
    0: 'CLR',  1: 'FAIR',  2: 'PCLD', 3: 'OVC',
    45: 'FOG', 48: 'FZFG',
    51: 'DZ',  53: 'DZ',   55: 'DZ+',
    56: 'FZDZ',57: 'FZDZ+',
    61: 'RA',  63: 'RA',   65: 'RA+',
    66: 'FZRA',67: 'FZRA+',
    71: 'SN',  73: 'SN',   75: 'SN+',
    77: 'SG',
    80: 'SHRA',81: 'SHRA', 82: 'SHRA+',
    85: 'SHSN',86: 'SHSN+',
    95: 'TS',  96: 'TSGR', 99: 'TSGR+'
  };
  return map.hasOwnProperty(code) ? map[code] : 'WX';
}

// Reverse-geocode lat/lon → short city name via Nominatim (free, no key)
function fetchCityName(lat, lon) {
  var url = 'https://nominatim.openstreetmap.org/reverse?format=json&lat=' +
            encodeURIComponent(lat) + '&lon=' + encodeURIComponent(lon) +
            '&zoom=10&addressdetails=1';
  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.setRequestHeader('User-Agent', 'LotseWatchface/2.3 pebble@localhost');
  xhr.onload = function() {
    if (xhr.status !== 200) {
      console.log('Nominatim HTTP ' + xhr.status);
      return;
    }
    try {
      var data    = JSON.parse(xhr.responseText);
      var addr    = data.address || {};
      // Prefer city > town > village > county, truncated to 8 chars for watch display
      var city    = addr.city || addr.town || addr.village || addr.county || '';
      if (city.length > 8) city = city.substring(0, 8);
      if (city) {
        var dict = {};
        dict[KEY_CITY] = city;
        sendToWatch(dict);
      }
    } catch(err) {
      console.log('Nominatim parse error: ' + err.message);
    }
  };
  xhr.onerror = function() { console.log('Nominatim request failed'); };
  xhr.send();
}

function fetchWeatherAt(lat, lon) {
  var url = 'https://api.open-meteo.com/v1/forecast?latitude=' +
            encodeURIComponent(lat) + '&longitude=' + encodeURIComponent(lon) +
            '&current=temperature_2m,weather_code&temperature_unit=celsius&windspeed_unit=kn';

  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.onload = function() {
    if (xhr.status !== 200) {
      console.log('Open-Meteo HTTP ' + xhr.status);
      sendToWatch({ 1: 'WX ERR' });
      return;
    }
    try {
      var data = JSON.parse(xhr.responseText);
      var cur  = data.current || {};
      var temp = Math.round(cur.temperature_2m);
      var cond = weatherCodeToShort(cur.weather_code);
      var dict = {};
      if (!isNaN(temp)) dict[KEY_TEMPERATURE] = temp;
      dict[KEY_CONDITIONS] = cond;
      sendToWatch(dict);
    } catch(err) {
      console.log('Weather parse error: ' + err.message);
      sendToWatch({ 1: 'WX ERR' });
    }
  };
  xhr.onerror = function() {
    console.log('Weather request failed');
    sendToWatch({ 1: 'WX ERR' });
  };
  xhr.send();

  // Also fetch city name (separate call, non-blocking)
  fetchCityName(lat, lon);
}

function fetchWeather() {
  if (!navigator.geolocation) {
    sendToWatch({ 1: 'NO GPS' });
    return;
  }
  navigator.geolocation.getCurrentPosition(function(pos) {
    fetchWeatherAt(pos.coords.latitude, pos.coords.longitude);
  }, function(err) {
    console.log('Geolocation error: ' + err.message);
    sendToWatch({ 1: 'LOC ERR' });
  }, {
    enableHighAccuracy: false,
    timeout: 15000,
    maximumAge: 30 * 60 * 1000
  });
}

function scheduleWeather() {
  if (weatherTimer) clearInterval(weatherTimer);
  weatherTimer = setInterval(fetchWeather, 30 * 60 * 1000);
}

Pebble.addEventListener('ready', function() {
  fetchWeather();
  scheduleWeather();
});

Pebble.addEventListener('appmessage', function(e) {
  if (e && e.payload && e.payload[KEY_REQUEST_WEATHER]) {
    fetchWeather();
  }
});