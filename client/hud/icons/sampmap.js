// Resolver pentru iconițe map (SA-MP ID 0-63 -> radar sprite pixel-art) și HUD outline.
// Pachetul: client/hud/icons/{hud,radar}/*.svg + manifest.json (SKR-SVG-Pack v1).
window.SKR_ICON_SRC = function (id) {
    if (typeof id === 'string') return 'icons/hud/' + id + '.svg';
    var n = (id | 0);
    if (n < 0) n = 0; else if (n > 63) n = 0;
    return 'icons/radar/' + (n < 10 ? '0' : '') + n + '.svg';
};
