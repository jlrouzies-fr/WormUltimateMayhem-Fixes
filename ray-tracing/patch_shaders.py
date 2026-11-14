"""
Edit copies of the game's shaders so that sunlight, ambient light, reflections and the sea's
mirror image come from the traced textures.

    python patch_shaders.py DIR      DIR holds unmodified FixedFunction.cg, Landscape.cg and water.cg

The harness (wumrt.c) feeds the extra uniforms; with them left at zero the shaders behave
exactly like the originals.
"""
from pathlib import Path
import sys

cg = Path(sys.argv[1])
s = ""


def rep(old, new, count=1):
    global s
    assert s.count(old) == count, (old[:70], s.count(old))
    s = s.replace(old, new)


HELPER = """
// --- ray-traced sun shadows, indirect light and reflections (wumrt) ------------
// rtMask holds, for every pixel of the frame, what was traced for the nearest shadow
// caster before the scene is drawn: x = how much of the sun it sees, y = its distance
// from the eye, z = how much of the sky it sees. rtBounce holds the sunlight that
// nearby surfaces reflect onto it (rgb). rtRefl holds what the surface mirrors (rgb),
// already weighted by how reflective its material is.
// rtParams = (enabled, 1/width, 1/height of the view, strength of bounced sunlight).
// rtParams2 = (1 if the mask has fewer pixels than the view, mask width, mask height,
// strength of the sky occlusion or 0 for none).
// A negative rtParams2.w means rtBounce holds all the indirect light (sky and surfaces),
// which then takes the place of the ambient colour.
// Returns the sun visibility; ambient gets the light to use instead of ambientCol, spec
// the mirrored light to add.
float RtLight( sampler2D rtMask, sampler2D rtBounce, sampler2D rtRefl, float4 rtParams, float4 rtParams2,
			   float4 wpos, float eyeDist, float3 ambientCol, float3 diffuseCol, out float3 ambient,
			   out float3 spec )
{
	float3 bounce = float3( 0, 0, 0 );
	float valid = 0;
	ambient = ambientCol;
	spec = float3( 0, 0, 0 );
	if ( rtParams.x < 0.5f )
		return 1;
	float2 uv = wpos.xy * rtParams.yz;
	float tol = 0.03f * eyeDist + 0.25f;
	float3 r = float3( 1, 0, 1 );
	if ( rtParams2.x < 0.5f )
	{
		float3 m = tex2D( rtMask, uv ).xyz;
		// If this surface is not the one the mask was traced for, leave it lit
		if ( abs( m.y - eyeDist ) <= tol )
		{
			r = m;
			bounce = tex2D( rtBounce, uv ).xyz;
			spec = tex2D( rtRefl, uv ).xyz;
			valid = 1;
		}
	}
	else
	{
		// Coarser mask: blend the four nearest mask pixels that lie on this surface
		float2 st = uv * rtParams2.yz - 0.5f;
		float2 base = floor( st );
		float2 f = st - base;
		float2 inv = 1.0f / rtParams2.yz;
		float3 sum = float3( 0, 0, 0 );
		float3 bsum = float3( 0, 0, 0 );
		float3 ssum = float3( 0, 0, 0 );
		float wsum = 0;
		for ( int j = 0; j < 2; j++ )
		{
			for ( int i = 0; i < 2; i++ )
			{
				float2 tc = ( base + float2( i, j ) + 0.5f ) * inv;
				float3 m = tex2D( rtMask, tc ).xyz;
				float w = ( i == 0 ? 1 - f.x : f.x ) * ( j == 0 ? 1 - f.y : f.y ) + 0.001f;
				if ( abs( m.y - eyeDist ) <= tol )
				{
					sum += m * w;
					bsum += tex2D( rtBounce, tc ).xyz * w;
					ssum += tex2D( rtRefl, tc ).xyz * w;
					wsum += w;
				}
			}
		}
		if ( wsum > 0 )
		{
			r = sum / wsum;
			bounce = bsum / wsum;
			spec = ssum / wsum;
			valid = 1;
		}
	}
	if ( rtParams2.w < 0 )
	{
		if ( valid > 0.5f )
			ambient = bounce;
	}
	else
		ambient = ambientCol * lerp( 1, r.z, rtParams2.w ) + diffuseCol * bounce * rtParams.w;
	return r.x;
}

// Normal mapping. rtNormal is a tangent-space normal map that goes with the material's
// colour texture (same coordinates); strength 0 means there is none. The meshes carry no
// tangents, so the surface's u and v directions are worked out per pixel from how position
// and texture coordinate change across the screen.
float3 RtBump( sampler2D rtNormal, float2 uv, float3 n, float3 pos, float strength )
{
	n = normalize( n );
	if ( strength == 0 )
		return n;
	float3 t = tex2D( rtNormal, uv ).xyz * 2 - 1;
	// The frame built below comes out mirrored for this game's position input (judged on a
	// carved door at 1080p: raised mouldings must catch light on their upper edges).
	t.xy = -t.xy;
	float3 dp1 = ddx( pos );
	float3 dp2 = ddy( pos );
	float2 du1 = ddx( uv );
	float2 du2 = ddy( uv );
	float3 dp2perp = cross( dp2, n );
	float3 dp1perp = cross( n, dp1 );
	float3 T = dp2perp * du1.x + dp1perp * du2.x;
	float3 B = dp2perp * du1.y + dp1perp * du2.y;
	float scale = rsqrt( max( dot( T, T ), dot( B, B ) ) + 1e-20f );
	return normalize( T * ( t.x * strength * scale ) + B * ( t.y * strength * scale ) + n * t.z );
}

// The indirect light was traced for the flat triangle under this pixel. On a smooth-shaded
// shape the shading normal differs from the triangle's, which shows as one brightness per
// triangle. This rescales the light for the smooth normal by the same hemisphere-light rule
// as RtHemi below; the triangle's own normal comes from how position changes across the
// screen. p3.yzw = the world's up direction in eye space (zero switches this off).
float RtSmooth( float3 ns, float3 pos, float4 p3 )
{
	float3 nf = normalize( cross( ddx( pos ), ddy( pos ) ) );
	ns = normalize( ns );
	if ( dot( nf, ns ) < 0 )
		nf = -nf;
	return clamp( ( 1 + dot( ns, p3.yzw ) ) / max( 1 + dot( nf, p3.yzw ), 0.3f ), 0.6f, 1.5f );
}

// How much more or less of the traced indirect light a normal-mapped surface point gets than
// the flat surface it was traced for. That light comes mostly from the sky, so a point
// tilted upwards gets more and one tilted downwards less (the hemisphere-light rule).
// p3 = rtParams3: x = normal map strength (0 = none), yzw = the world's up direction in eye space.
float RtHemi( float3 bumped, float3 flat, float4 p3 )
{
	if ( p3.x == 0 )
		return 1;
	return clamp( ( 1 + dot( bumped, p3.yzw ) ) / max( 1 + dot( normalize( flat ), p3.yzw ), 0.3f ), 0.5f, 1.6f );
}

// Parallax relief. The alpha of rtNormal holds a height (1 = the top of the relief; a normal
// map without heights is 1 everywhere, so nothing moves). The view ray is walked into the
// relief in texture space until it drops below the height, so carved detail shifts with the
// view and hides what lies behind it; from there a ray towards the light is walked back up,
// and relief in its way shades the point (self-shadowing).
// rtRelief = (depth of height 0 in texture repeats, self-shadow strength, 0, 0); x = 0 is off.
// pos: from the point towards the eye, l: towards the light, n: the surface normal (eye
// space, any length). Returns the texture coordinate to use; shadow scales the sunlight.
// Both loops have fixed bounds.
float2 RtRelief( sampler2D rtNormal, float2 uv, float3 n, float3 pos, float3 l, float4 rtRelief, out float shadow )
{
	shadow = 1;
	if ( rtRelief.x <= 0 )
		return uv;
	n = normalize( n );
	// pos points at the eye: its screen derivatives are those of the point's position, negated
	float3 dp1 = -ddx( pos );
	float3 dp2 = -ddy( pos );
	float2 du1 = ddx( uv );
	float2 du2 = ddy( uv );
	float det = du1.x * du2.y - du2.x * du1.y;
	if ( abs( det ) < 1e-14f )
		return uv;
	// how position changes per unit of u and of v, and the gradients of u and v along the surface
	float3 pu = ( dp1 * du2.y - dp2 * du1.y ) / det;
	float3 pv = ( dp2 * du1.x - dp1 * du2.x ) / det;
	float3 gu = cross( pv, n );
	gu /= dot( pu, gu );
	float3 gv = cross( n, pu );
	gv /= dot( pv, gv );
	float depth = rtRelief.x * sqrt( length( pu ) * length( pv ) );		// world units
	float3 v = -normalize( pos );		// the view ray, into the surface
	float vn = dot( v, n );
	if ( vn > -0.02f )
		return uv;
	// texture coordinate shift from the top of the relief to its bottom along the view ray
	float2 dive = float2( dot( v, gu ), dot( v, gv ) ) * ( depth / max( -vn, 0.25f ) );
	const int STEPS = 16;
	float prevRay = 1;
	float prevGap = 1 - tex2D( rtNormal, uv, du1, du2 ).a;		// ray height minus surface height
	float hitT = 1;
	for ( int i = 1; i <= STEPS; i++ )
	{
		float t = (float)i / STEPS;
		float ray = 1 - t;
		float gap = ray - tex2D( rtNormal, uv + dive * t, du1, du2 ).a;
		if ( gap <= 0 )
		{
			// between the last step above the surface and this one below it
			float f = prevGap / max( prevGap - gap, 1e-5f );
			hitT = ( (float)( i - 1 ) + f ) / STEPS;
			break;
		}
		prevGap = gap;
	}
	float2 hitUv = uv + dive * hitT;
	float hitH = 1 - hitT;
	float3 L = normalize( l );
	float ln = dot( L, n );
	if ( rtRelief.y > 0 && ln > 0.02f && hitH < 0.999f )
	{
		float2 rise = float2( dot( L, gu ), dot( L, gv ) ) * ( depth / max( ln, 0.2f ) );	// shift per full height
		float occ = 0;
		for ( int k = 1; k <= 8; k++ )
		{
			float ray = hitH + ( 1 - hitH ) * k / 8.0f;
			float above = tex2D( rtNormal, hitUv + rise * ( ray - hitH ), du1, du2 ).a - ray;
			occ = max( occ, above * ( 1 - k / 10.0f ) );
		}
		shadow = saturate( 1 - occ * rtRelief.y );
	}
	return hitUv;
}
"""

UNIFORMS = ("float4 inWPos : WPOS,\n\t\t\t\t\t\tuniform sampler2D rtMask,\n\t\t\t\t\t\tuniform sampler2D rtBounce,\n"
            "\t\t\t\t\t\tuniform sampler2D rtRefl,\n"
            "\t\t\t\t\t\tuniform float4 rtParams,\n\t\t\t\t\t\tuniform float4 rtParams2,\n"
            "\t\t\t\t\t\tuniform sampler2D rtNormal,\n\t\t\t\t\t\tuniform float4 rtParams3,\n"
            "\t\t\t\t\t\tuniform float4 rtRelief,\n")
CALL = "RtLight( rtMask, rtBounce, rtRefl, rtParams, rtParams2, inWPos, length( inPos ), %s, %s, rtAmbient, rtSpec )"
CALL_FF = CALL % ("lightAmbientCol", "lightDiffuseCol")
CALL_LS = (CALL % ("globalAmbient", "globalDiffuse")).replace("length( inPos )", "length( inPos ) + rtShellDepth")
DECL = "\tfloat3 rtAmbient;\n\tfloat3 rtSpec;\n\tfloat rtSun = "

# Shell layers for grass and moss (terrain only). See the comment inside.
SHELL_HELPER = """
// --- shell layers (wumrt) -------------------------------------------------------
// Grass and moss are drawn as a stack of copies of the terrain, each pushed a little further
// out along the surface normal, and every copy keeps only the pixels that lie inside a blade
// at its height. A blade is a cone standing in a cell of a grid laid over the texture
// coordinates: random place in the cell, random height, a lean that grows towards the tip,
// and one colour (the texture's at its foot). Two grids of different cell size and direction
// overlap, so the blades never line up in rows.
// The grids are sized in world units: how many texture repeats a world unit spans is worked
// out per pixel from how texture coordinate and position change across the screen, and
// rounded to a step of the square root of two so that it holds still.
// rtShell = (grid cells per world unit, this copy's height as a share of the full height
// (0 = the ground itself), the radius of a blade's foot in cells (0 = this texture carries no
// shells), the full height in world units: positive when the blades shade each other and the
// ground from the sun, negative when not).
// Returns 0 if the pixel is to be dropped; uv = where to read the colour texture, shade = a
// brightness factor (dark at the roots, light at the tips), sun = how much of the sun reaches
// the pixel past the other blades.
float3 RtShellHash( float2 c )
{
	float3 q = float3( dot( c, float2( 127.1f, 311.7f ) ), dot( c, float2( 269.5f, 183.3f ) ), dot( c, float2( 419.2f, 371.9f ) ) );
	return frac( sin( q ) * 43758.5453f );
}

// Wind and painted colour over the grass, both from w = a position on the surface in world
// units (texture coordinate times world units per texture unit). Both are a few sines: no
// loops, bounded. rtWind = (time in seconds, wind strength, colour patch strength, 0).
// How far the wind bends the blades right now at w, 0..1: waves of gusts roll across the
// field (about 180 units apart, 30 units a second), slowly swelling and fading.
float RtGust( float2 w, float time )
{
	float roll = 0.55f + 0.45f * sin( time * 1.1f - dot( w, float2( 0.8f, 0.6f ) ) * 0.035f );
	float swell = 0.7f + 0.3f * sin( time * 0.23f + dot( w, float2( -0.6f, 0.8f ) ) * 0.013f );
	return roll * swell;
}

// A colour factor for grass at w: big soft patches (40 to 80 units across) of drier, yellower
// grass and of lusher, bluer green, and a second set of patches a little darker or lighter.
float3 RtPatches( float2 w, float strength )
{
	float a = sin( w.x * 0.072f + sin( w.y * 0.05f ) * 1.7f ) * sin( w.y * 0.083f + sin( w.x * 0.043f + 1.3f ) * 1.9f );
	float b = sin( w.x * 0.034f + sin( w.y * 0.069f + 2.1f ) * 1.5f + 0.7f ) * sin( w.y * 0.038f - 0.4f );
	float3 dry = float3( 1.3f, 1.1f, 0.55f ), lush = float3( 0.78f, 0.97f, 1.0f );
	float3 tint = lerp( float3( 1, 1, 1 ), a > 0 ? dry : lush, saturate( abs( a ) * 2.2f ) * strength );
	return tint * ( 1 + 0.2f * b * strength );
}

// The tallest blade that covers grid point g at height t (a share of the full height), or -1;
// footOut = its foot in the grid, shade = its brightness there.
float RtShellCover( float2 g0, float t, float radius, float lod, float3 wind, float time, out float2 footOut, out float shade )
{
	footOut = g0;
	shade = 1;
	float best = -1;
	// The second grid is the first turned by 37 degrees with cells 1.31 times smaller
	const float2 rx = float2( 0.7986f, -0.6018f );
	const float2 ry = float2( 0.6018f, 0.7986f );
	for ( int k = 0; k < 2; k++ )
	{
		float2 g = g0;
		if ( k == 1 )
			g = float2( dot( g0, rx ), dot( g0, ry ) ) * 1.31f + 7.3f;
		float3 w = wind;
		if ( k == 1 )
			w.xy = float2( dot( wind.xy, rx ), dot( wind.xy, ry ) ) * 1.31f;
		// The four cells whose centres are nearest: a blade can reach into its neighbours
		float2 first = floor( g - 0.5f );
		for ( int j = 0; j < 2; j++ )
		{
			for ( int i = 0; i < 2; i++ )
			{
				float2 cell = first + float2( i, j );
				float3 rnd = RtShellHash( cell + k * 53.0f );
				float h = ( 0.35f + 0.65f * rnd.z ) * lod;
				float along = t / max( h, 0.001f );		// 0 at the foot, 1 at the tip
				float2 foot = cell + 0.5f + ( rnd.xy - 0.5f ) * 0.6f;
				float2 d = g - foot - ( rnd.yx - 0.5f ) * ( 0.5f * t * t );
				// wind: the blade bends along wind.xy (in this grid's own axes), more towards its
				// tip, by the gust plus a flutter of its own
				d -= w.xy * ( w.z * ( 1 + 0.25f * sin( time * 2.3f + rnd.x * 6.283f ) ) * t * t );
				float r = radius * ( 1 - along );
				// Of the blades that cover this pixel, show the tallest
				if ( along < 1 && dot( d, d ) < r * r && h > best )
				{
					best = h;
					if ( k == 1 )
					{
						foot = ( foot - 7.3f ) / 1.31f;
						foot = rx * foot.x + ry * foot.y;
					}
					footOut = foot;
					shade = ( 0.8f + 0.4f * rnd.x ) * lerp( 0.72f, 1.45f, along );
				}
			}
		}
	}
	return best;
}

float RtShell( float4 rtShell, float4 rtWind, float upY, float2 texUv, float3 pos, float3 n, float3 l, out float2 uv, out float3 shade, out float sun )
{
	uv = texUv;
	shade = 1;
	sun = 1;
	float coverShade = 1;
	if ( rtShell.z <= 0 )
		return 1;
	float t = rtShell.y;
	float2 dux = ddx( texUv );
	float2 duy = ddy( texUv );
	float3 dpx = ddx( pos );
	float3 dpy = ddy( pos );
	// Grid cells per texture repeat, along u and along v separately: from how far the surface
	// stretches per unit of u and of v, so that cells (and blades) are round in world units even
	// where the texture is stretched, as on steep faces.
	float repeats = sqrt( ( dot( dux, dux ) + dot( duy, duy ) ) / max( dot( dpx, dpx ) + dot( dpy, dpy ), 1e-12f ) );
	float2 cells = rtShell.x / max( repeats, 1e-6f );
	float det = dux.x * duy.y - duy.x * dux.y;
	float3 pu = 0, pv = 0;
	if ( abs( det ) > 1e-14f )
	{
		// how the surface moves per unit of u and of v (pos points at the eye: its derivatives
		// are the position's, negated)
		pu = ( dpy * dux.y - dpx * duy.y ) / det;
		pv = ( dpx * duy.x - dpy * dux.x ) / det;
		cells = rtShell.x * float2( length( pu ), length( pv ) );
		// never more than eight times as many cells one way as the other (degenerate mappings)
		float m = max( cells.x, cells.y ) / 8;
		cells = max( cells, m );
	}
	// Far away, where a cell would get smaller than about three pixels (it would only sparkle),
	// the grid gets coarser instead, in steps of the square root of two, so the blades cover the
	// whole map: bigger blades in the distance rather than none.
	float2 uvPerPixel = abs( dux ) + abs( duy );
	float perPixel = max( uvPerPixel.x * cells.x, uvPerPixel.y * cells.y );		// cells per pixel
	cells /= exp2( max( ceil( log2( max( perPixel, 1e-6f ) / 0.35f ) * 2 ), 0 ) * 0.5f );
	cells = exp2( floor( log2( cells ) * 2 + 0.5f ) * 0.5f );
	float2 g0 = texUv * cells;
	// The layers are flat slices: seen at a grazing angle their steps line up into visible
	// bands. A fixed noise over the surface (finer than a cell) moves each pixel's slice height
	// by up to half a layer, which breaks the bands into grain.
	if ( t > 0 )
		t = saturate( t + ( RtShellHash( floor( g0 * 6 ) + 91 ).x - 0.5f ) * 0.06f );
	const float lod = 1;
	// Sun shading from the other blades (rtShell.w > 0): a step from this point towards the sun,
	// up to a higher layer; if a blade stands there, it is in the way. Only the lower part of
	// the blades and the ground under them are tested (the tips see the sky).
	// Wind (rtWind.y > 0): a gust from the place on the surface, along a fixed direction of the
	// texture; up to 0.7 cells at the tip in the first grid (the second is turned and finer)
	float2 wpos = texUv * float2( length( pu ), length( pv ) );
	float3 wind0 = float3( 0.8f, 0.6f, rtWind.y > 0 ? rtWind.y * 0.7f * RtGust( wpos, rtWind.x ) : 0 );
	float2 footG, footS;
	float shadeS;
	float best = t > 0 ? RtShellCover( g0, t, min( rtShell.z, 0.45f ), lod, wind0, rtWind.x, footG, coverShade ) : 1;
	if ( best <= 0 )
		return 0;
	// ---- moss: moss on rock tops (rtWind.w > 0, rock textures; see wumrt_surfaces.ini) ----
	// Only where the rock faces up (upY = the world-up share of its normal) and a big soft
	// noise says so; there the blades are cut off at the mask's height, so the moss makes
	// soft cushions that thin out towards their edges, tinted green.
	float mossMask = 1;
	if ( rtWind.w > 0 )
	{
		float2 mw = wpos / 36;
		float mn = sin( mw.x * 1.7f + sin( mw.y * 1.3f ) * 1.6f ) * sin( mw.y * 1.9f + sin( mw.x * 1.1f + 0.8f ) * 1.4f );
		mossMask = saturate( smoothstep( 0.55f, 0.85f, upY ) * smoothstep( -0.25f, 0.35f, mn ) * rtWind.w );
		if ( t > mossMask * 0.999f )
			return 0;
	}
	// ---- end moss: moss on rock tops ----
	float ln = dot( l, n );
	if ( rtShell.w > 0 && t < 0.6f && ln > 0.05f && dot( pu, pu ) > 0 )
	{
		const float rise = 0.35f;
		float3 lt = l - n * ln;
		float uu = dot( pu, pu ), uv2 = dot( pu, pv ), vv = dot( pv, pv );
		float dt = uu * vv - uv2 * uv2;
		if ( dt > 1e-20f )
		{
			float a = ( dot( lt, pu ) * vv - dot( lt, pv ) * uv2 ) / dt;
			float b = ( dot( lt, pv ) * uu - dot( lt, pu ) * uv2 ) / dt;
			float2 shift = float2( a, b ) * cells * ( rise * rtShell.w / ln );
			float sl = length( shift );
			if ( sl > 3 )
				shift *= 3 / sl;
			if ( RtShellCover( g0 + shift, t + rise, min( rtShell.z, 0.45f ), lod, wind0, rtWind.x, footS, shadeS ) > 0 )
				sun = 0.3f;
		}
	}
	if ( t == 0 )
	{
		// The ground under the blades is in their shade: less where the blades' own sun shadows
		// already darken it
		shade = lerp( 1, rtShell.w > 0 ? 0.8f : 0.65f, lod );
		if ( rtWind.w > 0 )		// moss: rock ground is shaded and greened only under the moss
			shade = lerp( float3( 1, 1, 1 ), shade * float3( 0.8f, 1.0f, 0.65f ), mossMask );
		if ( rtWind.z > 0 )
			shade *= RtPatches( wpos, rtWind.z );
		return 1;
	}
	uv = footG / cells;
	shade = coverShade;
	if ( rtWind.w > 0 )		// moss: rock-coloured blades turned moss green
		shade *= float3( 0.62f, 1.12f, 0.42f );
	// a blade takes the patch colour at its foot, so it is one colour
	if ( rtWind.z > 0 )
		shade *= RtPatches( uv * float2( length( pu ), length( pv ) ), rtWind.z );
	return 1;
}
"""

# ---- FixedFunction.cg: objects ----
p = cg / "FixedFunction.cg"
s = p.read_text(encoding="latin1")
rep("float4x4 BlendMat[35]\t: register(c8);\n", "float4x4 BlendMat[35]\t: register(c8);\n" + HELPER)
rep("\t\t\t\t\t\tfloat specularPower,\n\t\t\t\t\t\tout float3 outBaseCol,\n\t\t\t\t\t\tout float3 outAdditiveCol )",
    "\t\t\t\t\t\tfloat specularPower,\n\t\t\t\t\t\tfloat rtSun,\n\t\t\t\t\t\tfloat3 rtAmbient,\n\t\t\t\t\t\tfloat3 rtSpec,\n"
    "\t\t\t\t\t\tout float3 outBaseCol,\n\t\t\t\t\t\tout float3 outAdditiveCol )")
rep("\tfloat diffuse = saturate( dot( n, l ) );\n", "\tfloat diffuse = saturate( dot( n, l ) ) * rtSun;\n")
rep("\t\telse\n\t\t\tspecular = 1;\n\n\toutBaseCol = lightDiffuseCol",
    "\t\telse\n\t\t\tspecular = 1;\n\tspecular *= rtSun;\n\n\toutBaseCol = lightDiffuseCol")
rep("\t\t\t\t lightAmbientCol * materialAmbientCol +\n", "\t\t\t\t rtAmbient * materialAmbientCol +\n")
rep("\toutAdditiveCol = lightSpecularCol * materialSpecularCol * specular;\n",
    "\toutAdditiveCol = lightSpecularCol * materialSpecularCol * specular + rtSpec;\n")
rep("\tCalculateLighting(\tinNormal, inPos, inLightVector,\n",
    DECL + CALL_FF + ";\n\tCalculateLighting(\tinNormal, inPos, inLightVector,\n", count=4)
# RtSmooth is not called: tried on 2026-11-14, it drew stripes along the worm's triangles
# (the traced light is already blended across triangles by the filter, so rescaling it per
# triangle puts the triangles back). Smooth normals have to come from the tracer instead.
rep("\t\t\t\t\t\tmaterialFresnelPower, specularPower,\n\t\t\t\t\t\tbaseCol, additiveCol );",
    "\t\t\t\t\t\tmaterialFresnelPower, specularPower, rtSun, rtAmbient, rtSpec,\n\t\t\t\t\t\tbaseCol, additiveCol );", count=4)
rep("uniform float4x4 materialMatrix,\n", "uniform float4x4 materialMatrix,\n\t\t\t\t\t\t" + UNIFORMS, count=4)
# The two textured variants light with the normal-mapped normal (rtParams3.x = strength).
for name in ("void FFFragmentMainTexLit(", "void FFFragmentMainTexLitCol("):
    start = s.index(name)
    call = "\tCalculateLighting(\tinNormal, inPos, inLightVector,\n"
    at = s.index(call, start)
    assert at - start < 3000, name
    s = (s[:at] + "\tfloat rtReliefShadow;\n"
         "\tfloat2 rtUv = RtRelief( rtNormal, inTexCoord0, inNormal, inPos, -inLightVector, rtRelief, rtReliefShadow );\n"
         "\trtSun *= rtReliefShadow;\n"
         "\tfloat3 rtN = RtBump( rtNormal, rtUv, inNormal, inPos, rtParams3.x );\n"
         "\trtAmbient *= RtHemi( rtN, inNormal, rtParams3 );\n"
         "\tCalculateLighting(\trtN, inPos, inLightVector,\n"
         + s[at + len(call):])
    # and the colour texture is read where the relief says
    old = "tex2D( texture0, inTexCoord0 )"
    at2 = s.index(old, start)
    assert at2 - start < 4000, name
    s = s[:at2] + "tex2D( texture0, rtUv, ddx( inTexCoord0 ), ddy( inTexCoord0 ) )" + s[at2 + len(old):]
p.write_text(s, encoding="latin1", newline="")


# Characters (wumrt_characters.ini): soft skin, light through thin parts, rim light, catchlights.
CHAR_HELPER = """
// --- characters (wumrt) -------------------------------------------------------------
// The worms are the stars of every shot, lit like animated-film characters:
// - light wrap: sunlight reaches a little past the line where the skin turns away from the
//   sun, warm-tinted, as in skin where light scatters under the surface;
// - light through thin parts: for skin turned away from the sun the tracer measured how much
//   body lies between the point and the sun (rtRefl.w = minus the share of light that gets
//   through, 0 where none does), and that glows warm;
// - rim light: a soft light along the edge the viewer sees, as strong as the point sees the
//   sky (the mask's z), so it does not glow in caves; warmer where the sun is behind;
// - catchlights: a small sharp highlight on the eyes, from above and a little from the sun.
// rtChar = (what this draw is: 0 = not a character, 1 = skin, 2 = skin whose texture also
// holds the eyes (their near-white texels), 3 = an eye; light wrap; rim light; light through
// thin parts), strengths 0 = off. rtChar2 = (catchlight strength, the world's up direction in
// eye space). The tracer's sun visibility on skin turned away from the sun is the far side's.
void RtSkin( float4 rtChar, float4 rtChar2, sampler2D rtMask, sampler2D rtRefl, float4 rtParams, float4 rtParams2,
			 float4 wpos, float eyeDist, float3 inNormal, float3 inCamera, float3 inLight, float3 lightDiffuseCol,
			 float3 lightAmbientCol, float rtSun, float3 texCol, inout float3 baseCol, inout float3 additiveCol )
{
	if ( rtChar.x < 0.5f || rtParams.x < 0.5f )
		return;
	float sky = 1;
	float trans = 0;
	float2 uv = wpos.xy * rtParams.yz;
	float tol = 0.03f * eyeDist + 0.25f;
	if ( rtParams2.x < 0.5f )
	{
		float4 m = tex2D( rtMask, uv );
		if ( abs( m.y - eyeDist ) <= tol )
		{
			sky = m.z;
			trans = max( -tex2D( rtRefl, uv ).w, 0 );
		}
	}
	else
	{
		float2 st = uv * rtParams2.yz - 0.5f;
		float2 base = floor( st );
		float2 f = st - base;
		float2 inv = 1.0f / rtParams2.yz;
		float2 sum = float2( 0, 0 );
		float wsum = 0;
		for ( int j = 0; j < 2; j++ )
		{
			for ( int i = 0; i < 2; i++ )
			{
				float2 tc = ( base + float2( i, j ) + 0.5f ) * inv;
				float4 m = tex2D( rtMask, tc );
				float w = ( i == 0 ? 1 - f.x : f.x ) * ( j == 0 ? 1 - f.y : f.y ) + 0.001f;
				if ( abs( m.y - eyeDist ) <= tol )
				{
					sum += float2( m.z, max( -tex2D( rtRefl, tc ).w, 0 ) ) * w;
					wsum += w;
				}
			}
		}
		if ( wsum > 0 )
		{
			sky = sum.x / wsum;
			trans = sum.y / wsum;
		}
	}
	float3 n = normalize( inNormal );
	float3 v = normalize( inCamera );
	float3 l = normalize( -inLight );
	float3 up = rtChar2.yzw;
	float ndl = dot( n, l );
	float behind = saturate( dot( -v, l ) );		// 1 when the sun is straight behind the subject
	const float3 warm = float3( 1.0f, 0.45f, 0.3f );
	if ( rtChar.w < 0 )
	{
		// Debug view (skin_translucency < 0): red = light through the body, green = sky
		// visibility, blue = sun visibility, as the tracer left them
		baseCol = float3( 0, 0, 0 );
		additiveCol = float3( trans, sky, rtSun );
		return;
	}
	if ( rtChar.x > 4.5f )
	{
		// Accessories (caps, helmets, hair): no skin light, but the same sky-fed rim as the
		// skin so the whole character reads as one shape, in a neutral colour, and a broad
		// soft sheen of the sun
		float rimA = pow( 1 - saturate( dot( n, v ) ), 3 ) * sky * ( 0.6f + 0.4f * saturate( dot( n, up ) + 0.5f ) );
		float3 rimColA = lerp( lightAmbientCol * 1.2f, lightDiffuseCol * rtSun, behind );
		baseCol += rimColA * ( rimA * rtChar.z );
		float3 hA = normalize( l + v );
		additiveCol += lightDiffuseCol * ( pow( saturate( dot( n, hA ) ), 12 ) * rtSun * 0.25f * rtChar.z );
		return;
	}
	if ( rtChar.x > 3.5f )
	{
		// Foliage (leaves, bushes, grass and flower cut-outs). Sunlight through the leaf where
		// the sun is behind it (the tracer found the far side lit), stronger looking into the
		// sun, tinted warm yellow-green: multiplied by the leaf texture it glows like a backlit
		// leaf in a film. A soft wrap keeps canopies from looking flat.
		const float3 glowTint = float3( 1.15f, 1.2f, 0.5f );
		baseCol += lightDiffuseCol * glowTint * ( trans * ( 0.35f + 0.65f * behind ) * rtChar.w * 1.5f );
		float wl = rtChar.y;
		float wrappedL = saturate( ( ndl + wl ) / ( 1 + wl ) );
		baseCol += lightDiffuseCol * float3( 0.9f, 1.0f, 0.6f ) * ( max( wrappedL - saturate( ndl ), 0 ) * rtSun );
		return;
	}
	if ( rtChar.x < 2.5f )
	{
		float w = rtChar.y;
		// Skin in shade stays warm (light that went under the skin comes out reddened): the
		// light falling on it keeps its brightness but takes a warm hue instead of the colour
		// of what is around (green grass, blue sky)
		const float3 LUM = float3( 0.3f, 0.59f, 0.11f );
		float shade = 1 - saturate( ndl ) * rtSun;
		float3 warmHue = float3( 1.18f, 0.94f, 0.82f );
		float3 warmed = dot( baseCol, LUM ) * warmHue / dot( warmHue, LUM );
		baseCol = lerp( baseCol, warmed, saturate( w ) * 0.8f * shade );
		float wrapped = saturate( ( ndl + w ) / ( 1 + w ) );
		baseCol += lightDiffuseCol * warm * ( max( wrapped - saturate( ndl ), 0 ) * rtSun );
		baseCol += lightDiffuseCol * warm * ( trans * saturate( -ndl * 3 ) * ( 0.4f + 0.6f * behind ) * rtChar.w );
		float rim = pow( 1 - saturate( dot( n, v ) ), 3 ) * sky * ( 0.6f + 0.4f * saturate( dot( n, up ) + 0.5f ) );
		float3 rimCol = lerp( lightAmbientCol * 1.3f, lightDiffuseCol * float3( 1.2f, 0.95f, 0.75f ) * rtSun, behind );
		baseCol += rimCol * ( rim * rtChar.z );
	}
	if ( rtChar.x > 2.5f && rtChar2.x > 0 )
	{
		// Catchlights: the pupil texture has a small glint painted in, which the game's lighting
		// dims to grey; it is shown at full brightness, as a film character's eyes catch light
		float glint = saturate( ( min( texCol.r, min( texCol.g, texCol.b ) ) - 0.6f ) / 0.2f );
		additiveCol += float3( 1, 1, 1 ) * ( glint * rtChar2.x * ( 0.6f + 0.4f * sky ) );
	}
}
"""
CHAR_UNIFORMS = "\t\t\t\t\t\tuniform float4 rtChar,\n\t\t\t\t\t\tuniform float4 rtChar2,\n"
p = cg / "FixedFunction.cg"
s = p.read_text(encoding="latin1")
rep("\nvoid CalculateLighting(", CHAR_HELPER + "\nvoid CalculateLighting(")
rep("\t\t\t\t\t\tuniform float4 rtParams3,\n", "\t\t\t\t\t\tuniform float4 rtParams3,\n" + CHAR_UNIFORMS, count=4)
for name, tex in (("void FFFragmentMainLit(", "float3( 0.5f, 0.5f, 0.5f )"),
                  ("void FFFragmentMainTexLit(", "tex2D( texture0, inTexCoord0 ).rgb"),
                  ("void FFFragmentMainLitCol(", "float3( 0.5f, 0.5f, 0.5f )"),
                  ("void FFFragmentMainTexLitCol(", "tex2D( texture0, inTexCoord0 ).rgb")):
    start = s.index(name)
    end = "\t\t\t\t\t\tbaseCol, additiveCol );\n"
    at = s.index(end, start) + len(end)
    assert at - start < 3500, name
    s = (s[:at] + "\tRtSkin( rtChar, rtChar2, rtMask, rtRefl, rtParams, rtParams2, inWPos, length( inPos ), inNormal, inPos,\n"
         "\t\t\tinLightVector, lightDiffuseCol, lightAmbientCol, rtSun, " + tex + ", baseCol, additiveCol );\n" + s[at:])
p.write_text(s, encoding="latin1", newline="")

# ---- FixedFunction.cg: smoke puffs lit like soft volumes (wumrt.c sets rtSmoke for smoke only) ----
# rtSmoke = (on, strength, texture step for the alpha slope, slope gain; negative flips v).
# rtSmokeSun = (direction towards the sun in view space, x right, y up, z towards the eye; visibility).
# rtSmokeSunCol = (sun colour, debug split: only pixels left of this x are lit, 0 = all); rtSmokeAmb = (ambient colour, how much the underside keeps).
# rtSmokeFx = (brightest effect light in view space, its radius), rtSmokeFxCol = (its power, z sign).
# The puff's alpha slope gives a normal that bends out at its edges and along its billows, so
# the sunny side is bright, the far side falls off softly and the underside is darker.
p = cg / "FixedFunction.cg"
s = p.read_text(encoding="latin1")
rep("""	FFVertexMainTex(	inPos, inTexCoord0,
						outPos, outTexCoord0,
						model, view, projection, texTransform );
	outColour = inColour;
}""", """	FFVertexMainTex(	inPos, inTexCoord0,
						outPos, outTexCoord0,
						model, view, projection, texTransform );
	outColour = inColour;
	outVPos = mul( mul( float4( inPos, 1 ), model ), view ).xyz;
}""")
rep("""							out float4 outColour : COLOR,
							uniform float4x4 model,
							uniform float4x4 view,
							uniform float4x4 projection,
							uniform float4x4 texTransform )
{
	FFVertexMainTex(""", """							out float4 outColour : COLOR,
							out float3 outVPos : TEXCOORD1,
							uniform float4x4 model,
							uniform float4x4 view,
							uniform float4x4 projection,
							uniform float4x4 texTransform )
{
	FFVertexMainTex(""")
rep("""void FFFragmentMainTexCol(	float2 inTexCoord0 : TEXCOORD0,
							float4 inCol : COLOR,
							uniform sampler2D texture0,
							out float4 outCol : COLOR )
{
	outCol = tex2D( texture0, inTexCoord0 ) * inCol;
}""", """void FFFragmentMainTexCol(	float2 inTexCoord0 : TEXCOORD0,
							float4 inCol : COLOR,
							float3 inVPos : TEXCOORD1,
							float4 inWPos : WPOS,
							uniform sampler2D texture0,
							uniform float4 rtSmoke,
							uniform float4 rtSmokeSun,
							uniform float4 rtSmokeSunCol,
							uniform float4 rtSmokeAmb,
							uniform float4 rtSmokeFx,
							uniform float4 rtSmokeFxCol,
							out float4 outCol : COLOR )
{
	outCol = tex2D( texture0, inTexCoord0 ) * inCol;
	if ( rtSmoke.x > 0.5f && ( rtSmokeSunCol.w <= 0 || inWPos.x < rtSmokeSunCol.w ) )
	{
		float d = rtSmoke.z;
		float ax = tex2D( texture0, inTexCoord0 + float2( d, 0 ) ).a - tex2D( texture0, inTexCoord0 - float2( d, 0 ) ).a;
		float ay = tex2D( texture0, inTexCoord0 + float2( 0, d ) ).a - tex2D( texture0, inTexCoord0 - float2( 0, d ) ).a;
		float3 n = normalize( float3( -ax * abs( rtSmoke.w ), -ay * rtSmoke.w, 1.0f ) );
		float wrap = saturate( ( dot( n, rtSmokeSun.xyz ) + 0.5f ) / 1.5f );
		float up = saturate( n.y * 0.7f + 0.5f );
		float3 light = rtSmokeAmb.rgb * lerp( rtSmokeAmb.w, 1.0f, up ) + rtSmokeSunCol.rgb * ( wrap * wrap * rtSmokeSun.w );
		float3 tl = rtSmokeFx.xyz - inVPos;
		float d2 = dot( tl, tl );
		tl.z *= rtSmokeFxCol.w;
		float fw = saturate( ( dot( n, tl * rsqrt( d2 + 1e-4f ) ) + 0.7f ) / 1.7f );
		light += rtSmokeFxCol.rgb * ( fw / ( d2 + rtSmokeFx.w * rtSmokeFx.w ) );
		outCol.rgb *= light * rtSmoke.y;
	}
}""")
p.write_text(s, encoding="latin1", newline="")

# ---- Landscape.cg: terrain ----
p = cg / "Landscape.cg"
s = p.read_text(encoding="latin1")
rep("// If we're in shadow, return zero, otherwise return 1\n", HELPER + SHELL_HELPER + "\n// If we're in shadow, return zero, otherwise return 1\n")
# Shell layers: the vertex programs push the mesh out along its normals by rtShell.x (0 for
# the game's own draw); the harness repeats grass draws with growing offsets.
SHELL_UNIFORM = "\t\t\t\t\t\tuniform float4 rtShell,\n"
WIND_UNIFORM = "\t\t\t\t\t\tuniform float4 rtWind,\n"
# characters stream: backlit blades, rtBlade = (glow strength, tip highlight, 0, 0)
WIND_UNIFORM += "\t\t\t\t\t\tuniform float4 rtBlade,\n"
rep("\t\t\t\t\t\tuniform float4x4 texTransform,\n\t\t\t\t\t\tuniform float4x4 shadowMatrix )\n{\n\tfloat4x4 modelView, modelViewProjection;\n",
    "\t\t\t\t\t\tuniform float4x4 texTransform,\n" + SHELL_UNIFORM +
    "\t\t\t\t\t\tuniform float4x4 shadowMatrix )\n{\n\tfloat4x4 modelView, modelViewProjection;\n"
    "\tinPos.xyz += inNormal * rtShell.x;\n"
    "\t// wumrt scatter draws: rtShell = (0, time, wind strength, 1). A tuft vertex's colour alpha\n"
    "\t// is 1 at the root and 0 at the tip; the tip sways with the same gusts as the shells.\n"
    "\tif ( rtShell.w > 0.5f )\n\t{\n"
    "\t\tfloat tip = 1 - inColour.a;\n"
    "\t\tfloat gust = ( 0.55f + 0.45f * sin( rtShell.y * 1.1f - dot( inPos.xz, float2( 0.8f, 0.6f ) ) * 0.035f ) )\n"
    "\t\t\t* ( 0.7f + 0.3f * sin( rtShell.y * 0.23f + dot( inPos.xz, float2( -0.6f, 0.8f ) ) * 0.013f ) );\n"
    "\t\tfloat flutter = 0.25f * sin( rtShell.y * 2.3f + inPos.x * 0.7f + inPos.z * 0.5f );\n"
    "\t\tinPos.xz += float2( 0.8f, 0.6f ) * ( tip * rtShell.z * 1.6f * ( gust + flutter ) );\n"
    "\t\tinColour.a = 1;\n\t}\n")
rep("\toutTexCoord0 = mul( inTexCoord0, texTransform );\n\toutColour = inColour;\n",
    "\toutTexCoord0 = mul( inTexCoord0, texTransform );\n"
    "\t// wumrt moss: how much the surface faces world up, for moss on rock tops (z is unused)\n"
    "\toutTexCoord0.z = normalize( mul( float4( inNormal, 0 ), model ).xyz ).y;\n"
    "\toutColour = inColour;\n")
rep("\t\t\t\t\t\tuniform float4x4 texTransform,\n\t\t\t\t\t\tuniform float4x4 shadowMatrix )\n{\n\tLandscapeVertexMain(",
    "\t\t\t\t\t\tuniform float4x4 texTransform,\n" + SHELL_UNIFORM +
    "\t\t\t\t\t\tuniform float4x4 shadowMatrix )\n{\n\tLandscapeVertexMain(")
rep("\t\t\t\t\t\t model, view, projection, globalLightDir, texTransform, shadowMatrix );",
    "\t\t\t\t\t\t model, view, projection, globalLightDir, texTransform, rtShell, shadowMatrix );")
rep("LandscapeFragmentMain(\tfloat4 inTexCoord0 : TEXCOORD0,", "LandscapeFragmentMain(\t" + UNIFORMS + SHELL_UNIFORM + WIND_UNIFORM + "\t\t\t\t\t\tfloat4 inTexCoord0 : TEXCOORD0,")
rep("\t// If the current pixel is outside the shadow map, then it's not in shadow\n\tif ( shadowPosWithDivide.x < 0",
    "\t// A blade pixel reads the traced light of the ground behind it, which lies this much further\n"
    "\t// along the line of sight\n"
    "\tfloat rtShellDepth = abs( rtShell.w ) * rtShell.y / max( abs( dot( normalize( inNormal ), normalize( inPos ) ) ), 0.25f );\n"
    + DECL + CALL_LS + ";\n"
    "\tif ( rtParams.x > 0.5f )\n\t{\n\t\t// Traced sun visibility replaces the shadow map\n\t\tfShadowScale = rtSun;\n\t}else\n"
    "\t// If the current pixel is outside the shadow map, then it's not in shadow\n\tif ( shadowPosWithDivide.x < 0")
rep("\tbaseCol = globalDiffuse * diffuse + globalAmbient;\n", "\tbaseCol = globalDiffuse * diffuse + rtAmbient;\n")
rep("\tadditiveCol = globalSpecular * materialSpecularCol * specular;\n",
    "\tadditiveCol = globalSpecular * materialSpecularCol * specular + rtSpec;\n")
rep("HeightMapFragmentMain(\tfloat4 inTexCoord0 : TEXCOORD0,", "HeightMapFragmentMain(\t" + UNIFORMS + SHELL_UNIFORM + WIND_UNIFORM + "\t\t\t\t\t\tfloat4 inTexCoord0 : TEXCOORD0,")
rep("\tLandscapeFragmentMain( inTexCoord0, inPos, inNormal,",
    "\tLandscapeFragmentMain( inWPos, rtMask, rtBounce, rtRefl, rtParams, rtParams2, rtNormal, rtParams3, rtRelief, rtShell, rtWind, rtBlade, inTexCoord0, inPos, inNormal,")
rep("\tfloat3 n = normalize( inNormal );\n",
    "\tfloat rtReliefShadow;\n"
    "\tfloat2 rtUv = RtRelief( rtNormal, inTexCoord0.xy, inNormal, inPos, -inLightVector, rtRelief, rtReliefShadow );\n"
    "\tfShadowScale *= rtReliefShadow * rtShellSun;\n"
    "\tfloat3 n = RtBump( rtNormal, rtUv, inNormal, inPos, rtParams3.x );\n"
    "\trtAmbient *= RtHemi( n, inNormal, rtParams3 );\n")
rep("\t// Test for being in shadow before doing the lighting calculations\n",
    "\tfloat2 rtShellUv;\n\tfloat3 rtShellShade;\n\tfloat rtShellSun;\n"
    "\tif ( RtShell( rtShell, rtWind, inTexCoord0.z, inTexCoord0.xy, inPos, normalize( inNormal ), normalize( -inLightVector ), rtShellUv, rtShellShade, rtShellSun ) < 0.5f )\n\t\tdiscard;\n\n"
    "\t// Test for being in shadow before doing the lighting calculations\n")
rep("\tfloat4 texCol = tex2D( texture0, inTexCoord0.xy );\n    outCol = float4( saturate( baseCol * texCol.rgb + additiveCol ), texCol.a ) * inVertexColour;",
    "\tfloat4 texCol = tex2D( texture0, rtShellUv + rtUv - inTexCoord0.xy, ddx( inTexCoord0.xy ), ddy( inTexCoord0.xy ) );\n"
    "\ttexCol.rgb *= rtShellShade;\n"
    # ---- characters stream: backlit blades (rtBlade; off unless blade_glow > 0) ----
    "\t// Backlit blades (characters stream): with the sun behind the grass as seen from the\n"
    "\t// camera, light comes through the blades, warm yellow-green, more towards the thin tips\n"
    "\t// and only where the traced sun reaches; plus a soft bright rim on the tips\n"
    "\tif ( rtBlade.x > 0 && rtShell.z > 0 && rtShell.y > 0 )\n\t{\n"
    "\t\tfloat3 rtBl = normalize( -inLightVector );\n"
    "\t\tfloat rtBehind = saturate( dot( -normalize( inPos ), rtBl ) );\n"
    "\t\tfloat rtThin = 0.35f + 0.65f * rtShell.y;\n"
    "\t\tfloat rtThrough = rtSun * lerp( 0.5f, 1.0f, rtShellSun ) * rtBehind * rtBehind * rtThin;\n"
    "\t\tbaseCol += globalDiffuse * float3( 1.15f, 1.2f, 0.5f ) * ( rtThrough * rtBlade.x );\n"
    "\t\tadditiveCol += globalDiffuse * texCol.rgb * float3( 1.1f, 1.1f, 0.7f ) * ( rtBlade.y * rtSun * rtBehind * saturate( rtShell.y * 3 - 2 ) );\n"
    "\t}\n"
    # ---- end characters stream block ----
    "    outCol = float4( saturate( baseCol * texCol.rgb + additiveCol ), texCol.a ) * inVertexColour;")
p.write_text(s, encoding="latin1", newline="")

# Stylised sea (water_style > 0): rtRefl.a = 1 + how far the camera ray runs under the water
# to the seabed (wumrt_trace.comp), so shallow water turns turquoise and deep water a saturated
# blue, a soft chunky foam band rolls in where the water is shallow (shores, around rocks), and
# the sun leaves bright glints on the ripples for the film post's bloom. The game's own water
# brightness scales it all, so night levels stay dark. Fixed-size loops only.
WATER_STYLE = r"""// wumrt: stylised sea (water_style)
float4 RtWaterStyle( float4 rtWater, float4 rtR, float4 rtR0, float fres, float3 game, float3 nm,
                     float3 eye, float2 uv, float time, sampler2D nmap )
{
	// Depth along the view ray (steady lookup; the ripple-shifted one if that missed the sea)
	float d = ( rtR0.a > 0.5f ? rtR0.a : rtR.a ) - 1;
	float h = d * abs( eye.y );                          // depth straight down
	float k = rtWater.x;
	// How bright the game paints its sea here (dark at night, bright by day)
	float lum = dot( game, float3( 0.2126f, 0.7152f, 0.0722f ) );
	float bright = clamp( lum / 0.32f, 0.15f, 1.3f );
	float deep = 1 - exp( -d / 32.0f );
	float3 shallowC = float3( 0.22f, 0.86f, 0.80f );
	float3 midC = float3( 0.05f, 0.52f, 0.72f );
	float3 deepC = float3( 0.04f, 0.28f, 0.60f );
	float3 body = lerp( lerp( shallowC, midC, saturate( deep * 2 ) ), deepC, saturate( deep * 2 - 1 ) );
	// Ripples shade the body a little (broad, not fine)
	body *= 0.88f + 0.24f * saturate( nm.y * 0.5f + 0.5f );
	body *= bright;
	// Dark (night, dusk) seas keep the game's own hue with the depth's light and dark: the bright
	// palette only fully takes over where the game paints its sea bright (day)
	float3 hue = game * ( dot( body, float3( 0.2126f, 0.7152f, 0.0722f ) ) / max( lum, 0.01f ) );
	body = lerp( hue, body, smoothstep( 0.3f, 0.6f, lum ) );
	// Less mirror than the game's Fresnel: on bright days the sky would hide the colours
	float3 c = lerp( lerp( game, body, saturate( k ) ), rtR.rgb, saturate( fres ) * lerp( 1.0f, 0.6f, saturate( k ) ) );
	// Shore foam: chunky soft blobs from a slow, coarse sample of the ripple map (its values sit
	// near 0.5, so they are spread to 0..1 first), in rings that roll in towards the shore and
	// thin out over about 10 units of depth, plus a broken rim right at the shore.
	float n1 = tex2D( nmap, uv * 0.11f + time * float2( 0.011f, 0.007f ) ).r;
	float n2 = tex2D( nmap, uv * 0.23f - time * float2( 0.006f, 0.013f ) ).g;
	float blob = saturate( ( n1 * 0.6f + n2 * 0.4f - 0.38f ) * 4.0f );
	float band = 0.5f + 0.5f * sin( h * 0.9f - time * 1.6f + blob * 3.0f );
	float near = saturate( 1 - h / 10.0f );
	float lip = saturate( 1 - h / 2.0f );
	float f = smoothstep( 0.42f, 0.6f, near * near * ( 0.5f + 0.5f * band ) * ( 0.55f + 0.65f * blob ) );
	f = max( f, smoothstep( 0.45f, 0.7f, lip + blob * 0.35f - 0.15f ) ) * ( d < 8000.0f ? 1.0f : 0.0f );
	float3 foamC = float3( 1.0f, 1.0f, 1.0f ) * clamp( bright * 1.05f, 0.3f, 1.4f );
	f *= saturate( k );
	c = lerp( c, foamC, f );
	// Sun glints on the ripples: a tight highlight of the sun, strong enough for bloom
	float3 n = normalize( float3( nm.x * 0.55f, 1.0f, nm.y * 0.55f ) );
	float3 hv = normalize( rtWater.yzw - eye );
	float g = pow( saturate( dot( n, hv ) ), 2000.0f );
	float day = saturate( rtWater.z * 4.0f ) * saturate( lum * 3.0f );
	c += float3( 1.0f, 0.95f, 0.82f ) * g * 6.0f * k * day * ( 1 - f );
	// a: foam and deep water are opaque (the game's sea is see-through, and over open sea only
	// a pale backdrop lies behind it); the clear shallows stay see-through to the seabed
	return float4( c, max( f, saturate( deep * 1.3f ) * saturate( k ) ) );
}

"""
# ---- edges (stream "edges"): sculpted soil where the terrain turns over -------------------
# Explosions cut the landscape into hard walls. Where the ground turns from up-facing into a
# wall over a tight convex bend (the cut's lip), the soil gets a soft, light bevel band that
# catches the sun; the wall below gets a darker, richer colour, concave creases darken, and
# lips/walls get a sky rim light. Curvature comes from screen derivatives of the mesh normal,
# so no extra geometry. rtRelief.z = strength (0 = off, < 0 = debug colours), .w = lip radius
# in world units (wumrt.ini edges / edges_radius). Kept in its own block for clean merges.
EDGE_HELPER = """
// --- wumrt edges: rounded, rim-lit soil lips on cut terrain ---------------------------------
// rel.z = strength (0 off, < 0 debug: red = lip, green = wall, blue = crease), rel.w = lip
// radius (world units); p1.x = traced light on; p3.yzw = world up in eye space (0 = unknown).
// pos: from the point towards the eye; nIn: mesh normal; n: bumped normal; v, l: towards the
// eye and the light; sun: sun visibility; amb: sky light; sunCol: sun colour.
void RtEdge( float4 rel, float4 p1, float4 p3, float3 pos, float3 nIn, float3 n, float3 v, float3 l,
             float sun, float3 amb, float3 sunCol, inout float3 col, inout float3 add )
{
	if ( rel.z == 0 || p1.x < 0.5f )
		return;
	float s = min( abs( rel.z ), 2.0f );
	float3 nn = normalize( nIn );
	// curvature along the screen: > 0 convex (about 1 / radius), < 0 concave
	float3 dpx = ddx( pos );
	float3 dpy = ddy( pos );
	float k = -( dot( ddx( nn ), dpx ) + dot( ddy( nn ), dpy ) ) / max( dot( dpx, dpx ) + dot( dpy, dpy ), 1e-6f );
	float kr = k * max( rel.w, 0.01f );
	float lip = smoothstep( 0.15f, 0.9f, kr );
	float crease = smoothstep( 0.15f, 0.9f, -kr );
	float hasUp = dot( p3.yzw, p3.yzw ) > 0.5f ? 1.0f : 0.0f;
	float u = dot( nn, p3.yzw );				// 1 flat ground, 0 wall, < 0 overhang
	float wall = hasUp * smoothstep( 0.4f, 0.85f, 1 - u );
	// the lip's band: the convex bend where the ground turns over towards the wall
	float band = lip * ( hasUp > 0.5f ? saturate( 1 - abs( u - 0.55f ) / 0.5f ) : 1.0f );
	if ( rel.z < 0 )
	{
		add = float3( lip, wall, crease );
		col = 0;
		return;
	}
	float a = saturate( s );
	// darker, richer soil inside the cut
	float lum = dot( col, float3( 0.3f, 0.59f, 0.11f ) );
	float3 rich = max( lerp( lum.xxx, col, 1.4f ), 0 ) * float3( 0.82f, 0.76f, 0.72f );
	col = lerp( col, rich, wall * ( 1 - band ) * a );
	col *= 1 - 0.3f * crease * a;
	// soft bevel highlight along the lip, strongest where it faces the sun
	float hl = band * s * ( 0.4f + 0.6f * saturate( dot( n, l ) ) * ( 0.3f + 0.7f * sun ) );
	col *= 1 + 0.4f * hl;
	add += 0.1f * hl * ( sunCol * sun + amb ) * col;
	// sky rim light on lips and walls
	float r = 1 - saturate( dot( n, v ) );
	add += ( r * r * r ) * s * 0.35f * max( band, 0.5f * wall ) * amb;
}
"""
p = cg / "Landscape.cg"
s = p.read_text(encoding="latin1")
rep("\n// If we're in shadow, return zero, otherwise return 1\n", EDGE_HELPER + "\n// If we're in shadow, return zero, otherwise return 1\n")
rep("\ttexCol.rgb *= rtShellShade;\n",
    "\ttexCol.rgb *= rtShellShade;\n"
    "\tRtEdge( rtRelief, rtParams, rtParams3, inPos, inNormal, n, v, normalize( -inLightVector ), fShadowScale,\n"
    "\t\trtAmbient, globalDiffuse, texCol.rgb, additiveCol );\n")
p.write_text(s, encoding="latin1", newline="")
# ---- end edges ----

# ---- water.cg: the sea mirrors what the tracer found ----
p = cg / "water.cg"
if p.exists():
    s = p.read_text(encoding="latin1")
    rep("\t\t\t\t\t\tuniform float4x4 combinedWaterParams,\n",
        "\t\t\t\t\t\tuniform float4x4 combinedWaterParams,\n"
        "\t\t\t\t\t\tfloat4 inWPos : WPOS,\n\t\t\t\t\t\tuniform sampler2D rtRefl,\n\t\t\t\t\t\tuniform float4 rtParams,\n\t\t\t\t\t\tuniform float4 rtWater,\n")
    rep("\toutCol = float4( vDiffuse + vSphericalDiffuse + vSphericalSpecular, nearOpacity );",
        "\t// rtRefl: what the traced mirror ray saw at this pixel (a = 1 where the sea was visible).\n"
        "\t// rtParams = (enabled, 1/width, 1/height of the view, strength). The ripples shift the lookup.\n"
        "\tfloat3 rtCol = vDiffuse + vSphericalDiffuse;\n\tfloat rtA = nearOpacity;\n"
        "\tif ( rtParams.x > 0.5f )\n\t{\n"
        "\t\tfloat2 rtUv = inWPos.xy * rtParams.yz;\n"
        "\t\tfloat4 rtR = tex2D( rtRefl, rtUv + vCombinedNormalMap.xy * 0.02f );\n"
        "\t\tif ( rtR.a < 0.5f )\n\t\t\trtR = tex2D( rtRefl, rtUv );\n"
        "\t\tif ( rtR.a > 0.5f )\n\t\t{\n"
        "\t\t\t// More mirror-like the flatter the view (Fresnel)\n"
        "\t\t\tfloat rtF = 1 - saturate( dot( -inEyeVec, inNormal ) );\n"
        "\t\t\trtF = 0.06f + 0.94f * rtF * rtF * rtF * rtF * rtF;\n"
        "\t\t\trtCol = lerp( vDiffuse, rtR.rgb, saturate( rtF * rtParams.w ) );\n"
        "\t\t\tif ( rtWater.x > 0 )\n\t\t\t{\n"
        "\t\t\t\tfloat4 rtWS = RtWaterStyle( rtWater, rtR, tex2D( rtRefl, rtUv ), rtF * rtParams.w, vDiffuse + vSphericalDiffuse,\n"
        "\t\t\t\t                      vCombinedNormalMap, inEyeVec, inTexCoord0.xy, pausedTime, texture1 );\n"
        "\t\t\t\trtCol = rtWS.rgb;\n\t\t\t\trtA = lerp( nearOpacity, 1.0f, rtWS.a );\n\t\t\t}\n"
        "\t\t}\n\t}\n"
        "\toutCol = float4( rtCol + vSphericalSpecular, rtA );")
    # Stylised animated-film sea (water_style in wumrt.ini; rtWater = (strength, towards the sun)).
    s = s.replace("void WaterFragmentMain(", WATER_STYLE + "void WaterFragmentMain(", 1)
    p.write_text(s, encoding="latin1", newline="")

# ---- matte-painting sky (wumrt.c sky_style; off = the game's shader) ----
# The sky is drawn with the unlit FFFragmentMainTex. With rtSky.x > 0 (set by the harness for the
# sky's own draws only) it becomes a clean painted gradient (the game's sky, softened, deeper and
# more saturated overhead, lighter and warmer at the horizon), a soft sun disc with a corona, and
# a layer of soft stylised clouds drifting slowly (value noise, 4 fixed octaves, no loops).
SKY_FN = """
// --- matte-painting sky (wumrt sky_style) --------------------------------------------
// rtSky = (strength 0 = off, cloud amount, seconds, night 0..1); rtSkyR / rtSkyU = world direction
// per unit of screen x / y (w = 2 / viewport width / height); rtSkyF = world direction at the screen
// centre (w = viewport x); rtSkySun = direction towards the sun (w = disc strength); rtSkySunCol =
// its colour (w = viewport y).
float RtSkyHash( float2 p ) { return frac( sin( dot( p, float2( 127.1f, 311.7f ) ) ) * 43758.5453f ); }
float RtSkyNoise( float2 p )
{
	float2 i = floor( p );
	float2 f = frac( p );
	f = f * f * ( 3 - 2 * f );
	return lerp( lerp( RtSkyHash( i ), RtSkyHash( i + float2( 1, 0 ) ), f.x ),
				 lerp( RtSkyHash( i + float2( 0, 1 ) ), RtSkyHash( i + float2( 1, 1 ) ), f.x ), f.y );
}
float3 RtSky( float3 base, float3 soft, float4 wpos, float4 rtSky, float4 rtSkyR, float4 rtSkyU, float4 rtSkyF,
			  float4 rtSkySun, float4 rtSkySunCol )
{
	float2 ndc = ( wpos.xy - float2( rtSkyF.w, rtSkySunCol.w ) ) * float2( rtSkyR.w, rtSkyU.w ) - 1;
	float3 d = normalize( rtSkyF.xyz + ndc.x * rtSkyR.xyz + ndc.y * rtSkyU.xyz );
	float k = saturate( rtSky.x );
	float night = rtSky.w;
	float h = saturate( d.y );
	// 1. clean: the game's painted sky with its fine detail softened
	float3 c = lerp( base, soft, 0.3f );
	float lum = dot( c, float3( 0.299f, 0.587f, 0.114f ) );
	// 2. gradient: deeper and more saturated overhead, lighter and warmer near the horizon
	float up = smoothstep( 0.0f, 0.55f, h );
	float3 deep = max( lerp( float3( lum, lum, lum ), c, 1.4f ), 0 ) * lerp( 0.9f, 0.82f, night );
	float3 sc = rtSkySunCol.rgb / max( max( rtSkySunCol.r, rtSkySunCol.g ), max( rtSkySunCol.b, 0.05f ) );
	float3 warm = c * lerp( float3( 1, 1, 1 ), sc, 0.1f ) + sc * ( 0.035f * ( 1 - 0.75f * night ) );
	float3 g = lerp( warm, deep, up );
	// 3. clouds: soft rounded puffs on a dome, drifting slowly
	float a = 0;
	float3 cloud = g;
	if ( rtSky.y > 0 && d.y > -0.05f )
	{
		float2 p = d.xz / ( h + 0.18f ) * 0.75f + rtSky.z * float2( 0.010f, 0.0045f );	// same heading as the cloud shadows (cloud_shadows: drift x, 0.45 x)
		float n = 0.5f * RtSkyNoise( p ) + 0.25f * RtSkyNoise( p * 2.03f + 1.7f ) +
				  0.125f * RtSkyNoise( p * 4.07f + 3.1f ) + 0.0625f * RtSkyNoise( p * 8.13f + 5.3f );
		n *= 1.0667f;
		float cover = 0.84f - 0.26f * rtSky.y;
		a = smoothstep( cover, cover + 0.06f, n ) * smoothstep( 0.08f, 0.32f, h );
		// thicker inside = shaded underside; lit from above and from the sun's side
		float thick = saturate( ( n - cover ) * 3.5f );
		float3 litCol = lerp( g, saturate( lum * 2.2f + 0.12f ) * float3( 1.0f, 0.98f, 0.95f ), 0.7f );
		float3 shadeCol = lerp( g, litCol, 0.5f ) * float3( 0.9f, 0.92f, 1.0f );
		cloud = lerp( litCol, shadeCol, thick * 0.7f );
		float s = saturate( dot( d, rtSkySun.xyz ) );
		cloud += rtSkySunCol.rgb * ( pow( s, 6.0f ) * 0.35f * ( 1 - thick ) ) * ( 1 - night );
	}
	float3 col = lerp( g, cloud, a * 0.75f );
	// 4. the sun: a soft glowing disc and corona (smaller and cooler for the moon), behind the clouds
	if ( rtSkySun.w > 0 )
	{
		float s = dot( d, rtSkySun.xyz );
		float r = lerp( 0.99955f, 0.99972f, night );
		float disc = smoothstep( r - 0.00035f, r, s );
		float corona = pow( saturate( s ), 1200.0f ) * 0.25f + pow( saturate( s ), 60.0f ) * 0.04f * ( 1 - night );
		float3 discCol = lerp( float3( 1.0f, 0.97f, 0.9f ), sc, 0.25f ) * lerp( 1.3f, 1.0f, night );
		col += ( discCol * disc * ( 1 - a ) + sc * corona * ( 1 - 0.6f * a ) ) * rtSkySun.w;
	}
	return lerp( base, col, k );
}

"""
p = cg / "FixedFunction.cg"
s = p.read_text(encoding="latin1")
rep("void FFFragmentMainTex(\tfloat2 inTexCoord0 : TEXCOORD0,\n"
    "\t\t\t\t\t\tuniform sampler2D texture0,\n"
    "\t\t\t\t\t\tout float4 outCol : COLOR )\n"
    "{\n"
    "\toutCol = tex2D( texture0, inTexCoord0 );\n"
    "}\n",
    SKY_FN +
    "void FFFragmentMainTex(\tfloat2 inTexCoord0 : TEXCOORD0,\n"
    "\t\t\t\t\t\tfloat4 inWPos : WPOS,\n"
    "\t\t\t\t\t\tuniform sampler2D texture0,\n"
    "\t\t\t\t\t\tuniform float4 rtSky,\n"
    "\t\t\t\t\t\tuniform float4 rtSkyR,\n"
    "\t\t\t\t\t\tuniform float4 rtSkyU,\n"
    "\t\t\t\t\t\tuniform float4 rtSkyF,\n"
    "\t\t\t\t\t\tuniform float4 rtSkySun,\n"
    "\t\t\t\t\t\tuniform float4 rtSkySunCol,\n"
    "\t\t\t\t\t\tout float4 outCol : COLOR )\n"
    "{\n"
    "\toutCol = tex2D( texture0, inTexCoord0 );\n"
    "\tif ( rtSky.x > 0 )\n"
    "\t\toutCol.rgb = RtSky( outCol.rgb, tex2Dbias( texture0, float4( inTexCoord0, 0, 2.5f ) ).rgb, inWPos, rtSky,\n"
    "\t\t\t\t\t\t\trtSkyR, rtSkyU, rtSkyF, rtSkySun, rtSkySunCol );\n"
    "}\n")
# the sky's meshes are drawn with the vertex-coloured variant (also the smoke stream's lit smoke, above)
rep("\t\t\t\t\t\t\tuniform float4 rtSmokeFxCol,\n"
    "\t\t\t\t\t\t\tout float4 outCol : COLOR )\n",
    "\t\t\t\t\t\t\tuniform float4 rtSmokeFxCol,\n"
    "\t\t\t\t\t\t\tfloat4 inWPos : WPOS,\n"
    "\t\t\t\t\t\t\tuniform float4 rtSky,\n"
    "\t\t\t\t\t\t\tuniform float4 rtSkyR,\n"
    "\t\t\t\t\t\t\tuniform float4 rtSkyU,\n"
    "\t\t\t\t\t\t\tuniform float4 rtSkyF,\n"
    "\t\t\t\t\t\t\tuniform float4 rtSkySun,\n"
    "\t\t\t\t\t\t\tuniform float4 rtSkySunCol,\n"
    "\t\t\t\t\t\t\tout float4 outCol : COLOR )\n")
rep("\t\toutCol.rgb *= light * rtSmoke.y;\n\t}\n}",
    "\t\toutCol.rgb *= light * rtSmoke.y;\n\t}\n"
    "\tif ( rtSky.x > 0 )\n"
    "\t\toutCol.rgb = RtSky( outCol.rgb, tex2Dbias( texture0, float4( inTexCoord0, 0, 2.5f ) ).rgb * inCol.rgb, inWPos,\n"
    "\t\t\t\t\t\t\trtSky, rtSkyR, rtSkyU, rtSkyF, rtSkySun, rtSkySunCol );\n"
    "}")
p.write_text(s, encoding="latin1", newline="")

# ---- HDR for the film post-processing (post.c, hdr = 1 in wumrt.ini) ----
# The lit shaders clamp their colour to 0..1. Into the game's 8-bit scene texture the hardware
# clamps the same way, so nothing changes there; with hdr = 1 the scene texture holds 16-bit
# floats and bright sunlit surfaces and highlights keep values above 1 for the tone curve
# and the bloom. Negative values are still cut, and a very large limit keeps out infinities.
HDR_FN = ("// wumrt: the lit colour, unclamped above (see the film post-processing)\n"
          "float3 RtHdr( float3 c ) { return clamp( c, 0.0f, 64.0f ); }\n\n")
for name in ("FixedFunction.cg", "Landscape.cg"):
    p = cg / name
    s = p.read_text(encoding="latin1")
    n = s.count("outCol = float4( saturate(")
    assert n >= 1, (name, n)
    s = s.replace("outCol = float4( saturate(", "outCol = float4( RtHdr(")
    s = HDR_FN + s
    p.write_text(s, encoding="latin1", newline="")
print("shader edits ok")
