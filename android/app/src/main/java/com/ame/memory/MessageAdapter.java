package com.ame.memory;

import android.graphics.Color;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.recyclerview.widget.RecyclerView;

import java.util.List;

/** Both conversation screens display text; image turns persist only their vision summary. */
final class MessageAdapter extends RecyclerView.Adapter<MessageAdapter.ViewHolder> {
    private final List<Message> messages;

    MessageAdapter(List<Message> messages) {
        this.messages = messages;
    }

    static final class ViewHolder extends RecyclerView.ViewHolder {
        final LinearLayout root;
        final TextView text;
        final ImageView image;

        ViewHolder(View view) {
            super(view);
            root = view.findViewById(R.id.msgRoot);
            text = view.findViewById(R.id.tvMsg);
            image = view.findViewById(R.id.ivMsg);
        }
    }

    @Override
    public ViewHolder onCreateViewHolder(ViewGroup parent, int viewType) {
        return new ViewHolder(LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_message, parent, false));
    }

    @Override
    public int getItemCount() {
        return messages.size();
    }

    @Override
    public void onBindViewHolder(ViewHolder holder, int position) {
        Message message = messages.get(position);
        boolean user = "user".equals(message.role);
        int gravity = user ? Gravity.END : Gravity.START;
        holder.text.setText(message.text);
        holder.image.setVisibility(View.GONE);
        holder.text.setGravity(gravity);
        holder.root.setGravity(gravity);
        holder.text.setBackgroundColor(Color.parseColor(user ? "#DCE3FF" : "#F2F2F2"));
    }
}
