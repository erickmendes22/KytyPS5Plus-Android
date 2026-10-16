package com.kytyps5.android.library

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import com.kytyps5.android.GameEntry

/**
 * Cover-grid cells (icon0.png + title + title id), the layout of Vita3K's AppsListScreen and RPCSX's
 * GamesScreen rebuilt for a platform GridView. Tap plays; the gear opens per-game settings.
 */
class GameGridAdapter(
    private val context: Context,
    private val covers: CoverLoader,
    private val coverPx: Int,
    private val onPlay: (GameEntry) -> Unit,
    private val onSettings: (GameEntry) -> Unit,
) : BaseAdapter() {

    var games: List<GameEntry> = emptyList()
        set(value) {
            field = value
            notifyDataSetChanged()
        }

    private val density = context.resources.displayMetrics.density
    private val Int.dp: Int get() = (this * density).toInt()

    private class Holder(val cover: ImageView, val placeholder: TextView, val title: TextView, val subtitle: TextView, val gear: TextView)

    override fun getCount(): Int = games.size
    override fun getItem(position: Int): GameEntry = games[position]
    override fun getItemId(position: Int): Long = games[position].path.hashCode().toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
        val view = convertView ?: createCell()
        val holder = view.tag as Holder
        val game = games[position]
        holder.title.text = game.title
        holder.subtitle.text = game.titleId ?: if (game.folder.isFile) "ZArchive" else ""
        holder.placeholder.text = initials(game.title)
        holder.placeholder.visibility = View.GONE
        covers.load(game.icon, holder.cover) { holder.placeholder.visibility = View.VISIBLE }
        view.setOnClickListener { onPlay(game) }
        view.setOnLongClickListener { onSettings(game); true }
        holder.gear.setOnClickListener { onSettings(game) }
        return view
    }

    private fun createCell(): View {
        val cover = ImageView(context).apply {
            scaleType = ImageView.ScaleType.CENTER_CROP
            clipToOutline = true
            background = GradientDrawable().apply {
                setColor(Color.rgb(40, 44, 56))
                cornerRadius = 10f * density
            }
        }
        val placeholder = TextView(context).apply {
            gravity = Gravity.CENTER
            textSize = 30f
            setTextColor(Color.argb(200, 255, 255, 255))
            setTypeface(typeface, Typeface.BOLD)
        }
        val title = TextView(context).apply {
            textSize = 14f
            maxLines = 2
            ellipsize = TextUtils.TruncateAt.END
            setTypeface(typeface, Typeface.BOLD)
        }
        val subtitle = TextView(context).apply {
            textSize = 11f
            alpha = 0.65f
            maxLines = 1
        }
        val gear = TextView(context).apply {
            text = "⚙"
            textSize = 20f
            gravity = Gravity.CENTER
            contentDescription = "Game settings"
            setPadding(6.dp, 0, 0, 0)
        }
        val coverFrame = FrameLayout(context).apply {
            addView(cover, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            addView(placeholder, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        }
        val texts = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            addView(title)
            addView(subtitle)
        }
        val caption = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(gear, LinearLayout.LayoutParams(36.dp, 36.dp))
        }
        return LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(6.dp, 6.dp, 6.dp, 6.dp)
            addView(coverFrame, LinearLayout.LayoutParams(coverPx, coverPx))
            addView(caption, LinearLayout.LayoutParams(coverPx, ViewGroup.LayoutParams.WRAP_CONTENT))
            tag = Holder(cover, placeholder, title, subtitle, gear)
        }
    }

    private fun initials(title: String): String =
        title.split(' ', '-', '_').filter { it.isNotBlank() }.take(2).joinToString("") { it.take(1).uppercase() }
}
